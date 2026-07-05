#!/usr/bin/env python3
"""
Analyze Monolith invocation JSONL logs.

This is a local, read-only analyzer for:
    Logs/yyyyMMdd/{proxy,action,query}.jsonl

It normalizes mixed v1/v2/v3 records, classifies low-value noise, and emits
reports that can be used as source evidence for Monolith action/schema work.

Usage from the Monolith plugin root:
    python Analyzer/analyze_invocation_logs.py --log-root Logs
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import heapq
import json
import math
import os
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, Iterator, List, Optional, Sequence, Tuple


SURFACE_FILES = ("proxy", "action", "query")
DEFAULT_FORMATS = ("markdown", "json")
DEFAULT_OUTPUT_ROOT = Path("Saved") / "Monolith" / "LogAnalysis"
NORMALIZED_SCHEMA_VERSION = 1
FINDINGS_SCHEMA_VERSION = 1
DEFAULT_EVIDENCE_LIMIT = 5
TOP_SAMPLE_LIMIT = 200
LARGE_RESULT_BYTES = 200_000
SLOW_CALL_MS = 5_000.0
VERY_SLOW_CALL_MS = 30_000.0
LONG_GAP_MS = 60_000.0
MIN_ERROR_RATE_COUNT = 10
MIN_DUPLICATE_RETRY_COUNT = 5
MIN_ESCAPE_HATCH_COUNT = 5
# Recency dimension (6B): the "recent" window is the last N present date folders
# (or, with --fix-boundary, every folder on/after a yyyyMMdd). still_open and the
# recency-adjusted score are derived from this split. Defaults to 3 to match the
# backlog spec's "latest 3 days" recency check.
DEFAULT_RECENT_DAYS = 3
# Categories whose ROI is driven by activity/cost (calls), not error volume, so
# their still_open is keyed on recent *calls* rather than recent *errors*. The
# CRG maintenance loop is the canonical example: ~0 errors but still bleeding
# wall-time daily, so "no recent errors" must NOT read as closed for these.
CALLS_METRIC_CATEGORIES = (
    "maintenance_loop",
    "large_result",
    "slow_action",
    "child_query_bottleneck",
)
# Recency status -> ROI weight applied to a finding's base score to form
# recency_score. still_open/regressed keep or boost the score; quiet/closed
# findings are damped; no_recent_data is uncertain (kept mid-weight, never 0).
RECENCY_WEIGHTS = {
    "regressed": 1.3,
    "still_open": 1.0,
    "no_recent_data": 0.5,
    "stable_quiet": 0.4,
    "newly_quiet": 0.15,
}
SYNTHETIC_ARGUMENT_MARKERS = (
    "__test_",
    "__missing_console_test.db",
    "queryhashmismatchprobe",
    "paramguard",
    "/game/tests/monolith",
    "/game/temp/test",
    "d:/monolithoutside",
    "outsideproject",
    "malformedpath",
    "badpathtest",
    "collisionalias",
    "source/doesnotexist/missing.h",
    "sc_graphbadpath",
    "sc_testcrossfade",
    "sc_validsource",
    "sm_testsoundmix",
    "sw_testtestwave",
    "sw_collision",
    "/game/tests/audio",
    "/game/temp/someclass",
    "/game/temp/somewave",
    "testlimitspeccue",
    "testmaxchildrencue",
    "testmalformed",
    "testlimitspecmetasound",
)

SYNTHETIC_ACTION_IDS = {
    # ActionGuidance intentionally probes typo/unknown-action recovery. These
    # rows should validate routing hints, not inflate operational high-error ROI.
    "monolith.monolih_discover",
    "monolith.statux",
    "monolith.guid",
    "maerial.any_action",
    "xyzzy_qwerty_fnord.plover",
}

SEVERITY_RANK = {
    "info": 0,
    "low": 1,
    "medium": 2,
    "high": 3,
    "critical": 4,
}


def utc_now_text() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def timestamp_for_path() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def stable_json(data: Any) -> str:
    try:
        return json.dumps(data, sort_keys=True, ensure_ascii=True, separators=(",", ":"))
    except TypeError:
        return json.dumps(str(data), ensure_ascii=True)


def sha16(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8", errors="replace")).hexdigest()[:16]


def shorten_path(path: Path, base: Path, include_paths: bool = False) -> str:
    if include_paths:
        return str(path)
    try:
        return str(path.resolve().relative_to(base.resolve())).replace("\\", "/")
    except ValueError:
        return path.name


def path_is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def coerce_float(value: Any) -> Optional[float]:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    return None


def coerce_int(value: Any, default: int = 0) -> int:
    if isinstance(value, bool):
        return default
    if isinstance(value, int):
        return value
    if isinstance(value, float) and math.isfinite(value):
        return int(value)
    try:
        return int(str(value))
    except Exception:
        return default


def get_nested(data: Dict[str, Any], *keys: str) -> Any:
    cur: Any = data
    for key in keys:
        if not isinstance(cur, dict):
            return None
        cur = cur.get(key)
    return cur


def first_number(data: Dict[str, Any], keys: Sequence[str]) -> Optional[float]:
    for key in keys:
        value = data.get(key)
        num = coerce_float(value)
        if num is not None:
            return num
    return None


def compact_message(text: Any, limit: int = 240) -> str:
    if text is None:
        return ""
    value = str(text).replace("\r", " ").replace("\n", " ").strip()
    if len(value) <= limit:
        return value
    return value[: limit - 3] + "..."


def split_csv_arg(value: str) -> List[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def parse_formats(value: str) -> Tuple[str, ...]:
    formats = tuple(split_csv_arg(value))
    allowed = {"markdown", "json", "csv"}
    unknown = sorted(set(formats) - allowed)
    if unknown:
        raise argparse.ArgumentTypeError("unknown format(s): {0}".format(", ".join(unknown)))
    return formats or DEFAULT_FORMATS


def parse_surfaces(value: str) -> Tuple[str, ...]:
    surfaces = tuple(split_csv_arg(value))
    allowed = set(SURFACE_FILES)
    unknown = sorted(set(surfaces) - allowed)
    if unknown:
        raise argparse.ArgumentTypeError("unknown file surface(s): {0}".format(", ".join(unknown)))
    return surfaces or SURFACE_FILES


def severity_at_least(value: str, minimum: str) -> bool:
    return SEVERITY_RANK.get(value, 0) >= SEVERITY_RANK.get(minimum, 0)


@dataclass
class Evidence:
    record_key: str
    source_file: str
    line_number: int
    trace_id: str
    surface: str
    namespace: str
    action: str
    status: str
    duration_ms: Optional[float] = None
    outcome: str = ""
    error_class: str = ""
    error_code: str = ""
    message: str = ""
    tags: Tuple[str, ...] = field(default_factory=tuple)
    payload_bytes: int = 0

    def to_json(self) -> Dict[str, Any]:
        data: Dict[str, Any] = {
            "record_key": self.record_key,
            "source_file": self.source_file,
            "line_number": self.line_number,
            "trace_id": self.trace_id,
            "surface": self.surface,
            "namespace": self.namespace,
            "action": self.action,
            "status": self.status,
        }
        if self.duration_ms is not None:
            data["duration_ms"] = round(self.duration_ms, 3)
        if self.outcome:
            data["outcome"] = self.outcome
        if self.error_class:
            data["error_class"] = self.error_class
        if self.error_code:
            data["error_code"] = self.error_code
        if self.message:
            data["message"] = self.message
        if self.tags:
            data["tags"] = list(self.tags)
        if self.payload_bytes:
            data["payload_bytes"] = self.payload_bytes
        return data


@dataclass
class CanonicalEvent:
    source_path: Path
    source_file: str
    line_number: int
    surface: str
    format_version: int
    record_key: str
    trace_id: str
    span_id: str
    parent_span_id: str
    previous_record_id: str
    process_instance_id: str
    session_key: str
    start_time: str
    end_time: str
    duration_ms: Optional[float]
    time_since_previous_ms: Optional[float]
    status: str
    namespace: str
    action: str
    tool_name: str
    action_key: str
    argument_fingerprint: str
    retry_signature: str
    return_summary: Dict[str, Any]
    agent_signal: Dict[str, Any]
    phase_timing: Dict[str, Any]
    environment: Dict[str, Any]
    child_process: Dict[str, Any]
    outcome: str
    error_class: str
    error_code: str
    message: str
    payload_bytes: int
    tags: Tuple[str, ...]
    noise_class: str
    escape_hatch_cluster: str
    parse_warnings: Tuple[str, ...] = field(default_factory=tuple)

    @property
    def date_key(self) -> str:
        parent = self.source_path.parent.name
        return parent if re.fullmatch(r"\d{8}", parent) else "direct"

    @property
    def is_error(self) -> bool:
        return self.status != "success"

    def evidence(self) -> Evidence:
        return Evidence(
            record_key=self.record_key,
            source_file=self.source_file,
            line_number=self.line_number,
            trace_id=self.trace_id,
            surface=self.surface,
            namespace=self.namespace,
            action=self.action,
            status=self.status,
            duration_ms=self.duration_ms,
            outcome=self.outcome,
            error_class=self.error_class,
            error_code=self.error_code,
            message=self.message,
            tags=self.tags,
            payload_bytes=self.payload_bytes,
        )

    def to_normalized_json(self) -> Dict[str, Any]:
        return {
            "schema_version": NORMALIZED_SCHEMA_VERSION,
            "source_file": self.source_file,
            "line_number": self.line_number,
            "surface": self.surface,
            "format_version": self.format_version,
            "record_key": self.record_key,
            "trace_id": self.trace_id,
            "span_id": self.span_id,
            "parent_span_id": self.parent_span_id,
            "previous_record_id": self.previous_record_id,
            "process_instance_id": self.process_instance_id,
            "session_key": self.session_key,
            "start_time": self.start_time,
            "end_time": self.end_time,
            "duration_ms": self.duration_ms,
            "time_since_previous_ms": self.time_since_previous_ms,
            "status": self.status,
            "namespace": self.namespace,
            "action": self.action,
            "tool_name": self.tool_name,
            "action_key": self.action_key,
            "argument_fingerprint": self.argument_fingerprint,
            "retry_signature": self.retry_signature,
            "outcome": self.outcome,
            "error_class": self.error_class,
            "error_code": self.error_code,
            "message": self.message,
            "payload_bytes": self.payload_bytes,
            "tags": list(self.tags),
            "noise_class": self.noise_class,
            "escape_hatch_cluster": self.escape_hatch_cluster,
            "parse_warnings": list(self.parse_warnings),
        }


@dataclass
class Finding:
    finding_id: str
    category: str
    severity: str
    confidence: float
    title: str
    recommendation: str
    rank: Optional[int] = None
    score: float = 0.0
    sample: Dict[str, Any] = field(default_factory=dict)
    evidence: List[Evidence] = field(default_factory=list)
    action_key: str = ""
    recency: Optional[Dict[str, Any]] = None

    def to_json(self) -> Dict[str, Any]:
        data: Dict[str, Any] = {
            "finding_id": self.finding_id,
            "category": self.category,
            "severity": self.severity,
            "confidence": round(self.confidence, 3),
            "title": self.title,
            "recommendation": self.recommendation,
            "evidence": [item.to_json() for item in self.evidence],
        }
        if self.rank is not None:
            data["rank"] = self.rank
        if self.score:
            data["score"] = round(self.score, 3)
        if self.sample:
            data["sample"] = self.sample
        # Recency dimension (6B). Additive: existing consumers ignore the extra
        # keys. still_open is the headline flag (True/False/None where None means
        # no recent calls -> cannot conclude closed, per the spec's no-data caveat).
        if self.recency is not None:
            data["recency"] = self.recency
            data["still_open"] = self.recency.get("still_open")
            data["recency_status"] = self.recency.get("status")
            data["recency_score"] = self.recency.get("recency_score", round(self.score, 3))
        return data


class TopN:
    def __init__(self, limit: int) -> None:
        self.limit = max(1, limit)
        self._heap: List[Tuple[float, int, Any]] = []
        self._counter = 0

    def add(self, score: float, item: Any) -> None:
        self._counter += 1
        entry = (float(score), self._counter, item)
        if len(self._heap) < self.limit:
            heapq.heappush(self._heap, entry)
        elif score > self._heap[0][0]:
            heapq.heapreplace(self._heap, entry)

    def items_desc(self) -> List[Any]:
        return [item for _score, _idx, item in sorted(self._heap, reverse=True)]


class DurationStats:
    def __init__(self, sample_limit: int = 4000) -> None:
        self.count = 0
        self.total = 0.0
        self.min_value: Optional[float] = None
        self.max_value: Optional[float] = None
        self._samples: List[float] = []
        self._sample_limit = sample_limit

    def add(self, value: Optional[float]) -> None:
        if value is None:
            return
        self.count += 1
        self.total += value
        self.min_value = value if self.min_value is None else min(self.min_value, value)
        self.max_value = value if self.max_value is None else max(self.max_value, value)
        # Deterministic bounded sampling: keep all early samples, then keep every
        # Nth approximate bucket. This is not a statistical reservoir, but avoids
        # unbounded memory while preserving enough data for local triage.
        if len(self._samples) < self._sample_limit:
            self._samples.append(value)
        else:
            bucket = max(1, self.count // self._sample_limit)
            if self.count % bucket == 0:
                self._samples[self.count % self._sample_limit] = value

    def percentile(self, pct: float) -> float:
        if not self._samples:
            return 0.0
        vals = sorted(self._samples)
        idx = int((len(vals) - 1) * pct)
        return vals[idx]

    def as_row(self) -> Dict[str, Any]:
        return {
            "count": self.count,
            "total_ms": round(self.total, 3),
            "avg_ms": round(self.total / self.count, 3) if self.count else 0.0,
            "min_ms": round(self.min_value or 0.0, 3),
            "p50_ms": round(self.percentile(0.50), 3),
            "p90_ms": round(self.percentile(0.90), 3),
            "p95_ms": round(self.percentile(0.95), 3),
            "p99_ms": round(self.percentile(0.99), 3),
            "max_ms": round(self.max_value or 0.0, 3),
        }


class EvidenceBucket:
    def __init__(self, limit: int = DEFAULT_EVIDENCE_LIMIT) -> None:
        self.limit = limit
        self.items: List[Evidence] = []

    def add(self, event: CanonicalEvent) -> None:
        if len(self.items) < self.limit:
            self.items.append(event.evidence())


class Analyzer:
    def __init__(self, repo_root: Path, args: argparse.Namespace) -> None:
        self.repo_root = repo_root.resolve()
        self.args = args
        self.generated_at = utc_now_text()
        self.records_scanned = 0
        self.files_scanned = 0
        self.parse_warnings: List[Dict[str, Any]] = []
        self.count_by_file: Counter[str] = Counter()
        self.count_by_date_surface: Counter[Tuple[str, str]] = Counter()
        self.count_by_surface_version: Counter[Tuple[str, int]] = Counter()
        self.count_by_surface_status: Counter[Tuple[str, str]] = Counter()
        self.count_by_action: Counter[str] = Counter()
        self.error_by_action: Counter[str] = Counter()
        self.finding_count_by_action: Counter[str] = Counter()
        self.finding_error_by_action: Counter[str] = Counter()
        self.status_by_action: Counter[Tuple[str, str]] = Counter()
        self.outcome_by_action: Counter[Tuple[str, str]] = Counter()
        self.tag_by_action: Counter[Tuple[str, str]] = Counter()
        self.noise_counts: Counter[str] = Counter()
        self.noise_by_action: Counter[Tuple[str, str]] = Counter()
        self.index_health_counts: Counter[str] = Counter()
        self.headless_counts: Counter[str] = Counter()
        self.profile_counts: Counter[str] = Counter()
        self.environment_issue_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.retry_counts: Counter[Tuple[str, str]] = Counter()
        self.retry_evidence: Dict[Tuple[str, str], EvidenceBucket] = defaultdict(EvidenceBucket)
        self.duration_by_action: Dict[str, DurationStats] = defaultdict(DurationStats)
        self.payload_by_action: Counter[str] = Counter()
        self.max_payload_by_action: Dict[str, Tuple[int, Evidence]] = {}
        self.slow_calls = TopN(TOP_SAMPLE_LIMIT)
        self.large_results = TopN(TOP_SAMPLE_LIMIT)
        self.long_gaps = TopN(TOP_SAMPLE_LIMIT)
        self.child_process_calls = TopN(TOP_SAMPLE_LIMIT)
        self.child_process_count: Counter[str] = Counter()
        self.child_process_duration: Counter[str] = Counter()
        self.child_process_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.error_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.finding_error_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.schema_groups: Counter[str] = Counter()
        self.schema_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.unknown_groups: Counter[str] = Counter()
        self.unknown_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.escape_hatch_count: Counter[str] = Counter()
        self.escape_hatch_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.escape_hatch_clusters: Counter[Tuple[str, str]] = Counter()
        self.escape_hatch_cluster_errors: Counter[Tuple[str, str]] = Counter()
        self.escape_hatch_cluster_evidence: Dict[Tuple[str, str], EvidenceBucket] = defaultdict(EvidenceBucket)
        self.maintenance_count: Counter[str] = Counter()
        self.maintenance_duration: Counter[str] = Counter()
        self.maintenance_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.expected_slow_count: Counter[str] = Counter()
        self.expected_slow_errors: Counter[str] = Counter()
        self.expected_slow_duration: Counter[str] = Counter()
        self.expected_slow_evidence: Dict[str, EvidenceBucket] = defaultdict(EvidenceBucket)
        self.trace_versions: Dict[str, Counter[int]] = defaultdict(Counter)
        self.trace_events: Dict[str, int] = defaultdict(int)
        self.mixed_schema_days: Dict[str, Counter[int]] = defaultdict(Counter)
        # Recency dimension (6B): per-(action, date) call/error tallies feed the
        # last-K-days vs rest split. Kept separate from lifetime counters so
        # existing aggregates are untouched.
        self.count_by_action_date: Counter[Tuple[str, str]] = Counter()
        self.error_by_action_date: Counter[Tuple[str, str]] = Counter()
        self.all_date_keys: set = set()
        self._recent_dates_cache: Optional[set] = None
        self._action_date_index: Optional[Dict[str, Dict[str, List[int]]]] = None
        self.normalized_output = None

    def analyze_files(self, files: Sequence[Path]) -> None:
        if self.args.emit_normalized_jsonl:
            normalized_path = self.args.out / "normalized.jsonl"
            self.normalized_output = normalized_path.open("w", encoding="utf-8", newline="\n")
        try:
            for path in files:
                self.files_scanned += 1
                self._scan_file(path)
        finally:
            if self.normalized_output is not None:
                self.normalized_output.close()

    def _scan_file(self, path: Path) -> None:
        try:
            with path.open("r", encoding="utf-8", errors="replace") as handle:
                for line_number, line in enumerate(handle, 1):
                    if self.args.max_lines and self.records_scanned >= self.args.max_lines:
                        return
                    if not line.strip():
                        continue
                    try:
                        raw = json.loads(line)
                    except Exception as exc:
                        self._record_parse_warning(path, line_number, "json_parse", str(exc))
                        if self.args.strict:
                            raise StrictParseError(path, line_number, exc)
                        continue
                    event = normalize_event(raw, path, line_number, self.repo_root, self.args.include_paths)
                    self._record_event(event)
                    if self.normalized_output is not None:
                        self.normalized_output.write(json.dumps(event.to_normalized_json(), ensure_ascii=False) + "\n")
        except StrictParseError:
            raise
        except OSError as exc:
            self._record_parse_warning(path, 0, "file_read", str(exc))
            if self.args.strict:
                raise

    def _record_parse_warning(self, path: Path, line_number: int, kind: str, message: str) -> None:
        item = {
            "source_file": shorten_path(path, self.repo_root, self.args.include_paths),
            "line_number": line_number,
            "kind": kind,
            "message": compact_message(message),
        }
        if len(self.parse_warnings) < 1000:
            self.parse_warnings.append(item)

    def _record_event(self, event: CanonicalEvent) -> None:
        self.records_scanned += 1
        self.count_by_file[event.source_file] += 1
        self.count_by_date_surface[(event.date_key, event.surface)] += 1
        self.count_by_surface_version[(event.surface, event.format_version)] += 1
        self.count_by_surface_status[(event.surface, event.status)] += 1
        self.count_by_action[event.action_key] += 1
        self.status_by_action[(event.action_key, event.status)] += 1
        self.noise_counts[event.noise_class] += 1
        self.noise_by_action[(event.noise_class, event.action_key)] += 1
        date_key = event.date_key
        # all_date_keys defines the recent window over every real log day (noise
        # included), so the window is stable; the per-action recency tallies below
        # are gated to the same population as the findings they annotate.
        if date_key != "direct":
            self.all_date_keys.add(date_key)
        self._record_environment(event)
        self.duration_by_action[event.action_key].add(event.duration_ms)
        self.payload_by_action[event.action_key] += event.payload_bytes
        self.mixed_schema_days[event.date_key][event.format_version] += 1
        if event.trace_id:
            self.trace_versions[event.trace_id][event.format_version] += 1
            self.trace_events[event.trace_id] += 1

        if event.status != "success":
            self.error_by_action[event.action_key] += 1
            self.error_evidence[event.action_key].add(event)

        if not self._exclude_from_problem_findings(event):
            self.finding_count_by_action[event.action_key] += 1
            # Recency per-date tallies share the finding population so the
            # recent/historical split and recency_score weight are computed over
            # the same rows the finding's base score is (heartbeat/synthetic
            # excluded unless --include-* is passed); otherwise an action whose
            # only recent traffic is synthetic CI would flip no_recent_data -> closed.
            self.count_by_action_date[(event.action_key, date_key)] += 1
            if event.status != "success":
                self.finding_error_by_action[event.action_key] += 1
                self.finding_error_evidence[event.action_key].add(event)
                self.error_by_action_date[(event.action_key, date_key)] += 1

        if event.outcome:
            self.outcome_by_action[(event.action_key, event.outcome)] += 1

        for tag in event.tags:
            self.tag_by_action[(event.action_key, tag)] += 1

        if event.retry_signature:
            retry_key = (event.action_key, event.retry_signature)
            self.retry_counts[retry_key] += 1
            self.retry_evidence[retry_key].add(event)

        if event.duration_ms is not None and event.duration_ms >= SLOW_CALL_MS:
            self.slow_calls.add(event.duration_ms, event.evidence())

        if event.time_since_previous_ms is not None and event.time_since_previous_ms >= LONG_GAP_MS:
            self.long_gaps.add(event.time_since_previous_ms, event.evidence())

        if event.payload_bytes >= LARGE_RESULT_BYTES:
            self.large_results.add(event.payload_bytes, event.evidence())

        max_payload = self.max_payload_by_action.get(event.action_key)
        if event.payload_bytes and (max_payload is None or event.payload_bytes > max_payload[0]):
            self.max_payload_by_action[event.action_key] = (event.payload_bytes, event.evidence())

        child_ms = child_process_duration(event.child_process)
        if child_ms is not None:
            self.child_process_calls.add(child_ms, event.evidence())
            self.child_process_count[event.action_key] += 1
            self.child_process_duration[event.action_key] += child_ms
            self.child_process_evidence[event.action_key].add(event)

        if is_schema_confusion(event) and not self._exclude_from_problem_findings(event):
            key = schema_group_key(event)
            self.schema_groups[key] += 1
            self.schema_evidence[key].add(event)

        if is_unknown_action(event):
            if self.args.include_synthetic_tests or event.noise_class != "synthetic_test":
                key = unknown_group_key(event)
                self.unknown_groups[key] += 1
                self.unknown_evidence[key].add(event)

        if event.noise_class == "escape_hatch":
            self.escape_hatch_count[event.action_key] += 1
            self.escape_hatch_evidence[event.action_key].add(event)
            cluster_key = (event.action_key, event.escape_hatch_cluster or "other")
            self.escape_hatch_clusters[cluster_key] += 1
            if event.status != "success":
                self.escape_hatch_cluster_errors[cluster_key] += 1
            self.escape_hatch_cluster_evidence[cluster_key].add(event)

        if event.noise_class == "maintenance":
            self.maintenance_count[event.action_key] += 1
            if event.duration_ms is not None:
                self.maintenance_duration[event.action_key] += event.duration_ms
            self.maintenance_evidence[event.action_key].add(event)

        if event.noise_class == "expected_slow_domain":
            self.expected_slow_count[event.action_key] += 1
            if event.status != "success":
                self.expected_slow_errors[event.action_key] += 1
            if event.duration_ms is not None:
                self.expected_slow_duration[event.action_key] += event.duration_ms
            self.expected_slow_evidence[event.action_key].add(event)

    def _exclude_from_problem_findings(self, event: CanonicalEvent) -> bool:
        if event.noise_class == "heartbeat" and not self.args.include_heartbeats:
            return True
        if event.noise_class == "synthetic_test" and not self.args.include_synthetic_tests:
            return True
        return False

    def _record_environment(self, event: CanonicalEvent) -> None:
        env = event.environment
        if not env:
            return
        index_health = str(env.get("index_health") or "").strip().lower()
        if index_health:
            self.index_health_counts[index_health] += 1
            if index_health not in {"ok", "unknown"}:
                self.environment_issue_evidence["index_health:{0}".format(index_health)].add(event)
        if "headless" in env:
            self.headless_counts[str(env.get("headless"))] += 1
        if env.get("active_profile_id"):
            self.profile_counts[str(env.get("active_profile_id"))] += 1

    def _recent_date_set(self) -> set:
        """Dates that count as 'recent' for the recency split.

        With --fix-boundary, every dated folder on or after the boundary; else
        the last --recent-days *present* date folders (robust to gaps). 'direct'
        (non-dated) files are never recent.
        """
        if self._recent_dates_cache is not None:
            return self._recent_dates_cache
        dates = sorted(self.all_date_keys)
        if self.args.fix_boundary:
            recent = {d for d in dates if d >= self.args.fix_boundary}
        elif dates:
            recent = set(dates[-max(1, self.args.recent_days):])
        else:
            recent = set()
        self._recent_dates_cache = recent
        return recent

    def _action_date_index_build(self) -> Dict[str, Dict[str, List[int]]]:
        if self._action_date_index is None:
            index: Dict[str, Dict[str, List[int]]] = defaultdict(dict)
            for (action_key, date_key), count in self.count_by_action_date.items():
                errors = self.error_by_action_date.get((action_key, date_key), 0)
                index[action_key][date_key] = [count, errors]
            self._action_date_index = index
        return self._action_date_index

    def compute_recency(self, action_key: str, metric: str = "errors") -> Dict[str, Any]:
        """Split an action's calls/errors into recent vs historical and classify.

        metric='errors' (default): still_open keyed on recent *errors*; 0 recent
        calls -> no_recent_data/None (the spec's no-data-vs-fixed caveat). Used by
        error-shaped findings (schema, high-error, unknown-action, retry, slow-domain).

        metric='calls': still_open keyed on recent *calls* — used by cost/activity
        findings (maintenance loop, large result) where errors are ~0 but the
        expensive activity itself is what must stop.
        """
        index = self._action_date_index_build().get(action_key, {})
        recent_dates = self._recent_date_set()
        recent_calls = recent_errors = 0
        hist_calls = hist_errors = 0
        last_call_date = ""
        for date_key, (calls, errors) in index.items():
            if date_key != "direct" and (not last_call_date or date_key > last_call_date):
                last_call_date = date_key
            if date_key in recent_dates:
                recent_calls += calls
                recent_errors += errors
            else:
                hist_calls += calls
                hist_errors += errors
        recent_error_rate = recent_errors / recent_calls if recent_calls else 0.0
        hist_error_rate = hist_errors / hist_calls if hist_calls else 0.0

        if metric == "calls":
            if recent_calls > 0:
                still_open: Optional[bool] = True
                status = "still_open"
            else:
                still_open = False
                status = "newly_quiet" if hist_calls > 0 else "stable_quiet"
        else:
            if recent_calls == 0:
                still_open = None
                status = "no_recent_data"
            elif recent_errors == 0:
                still_open = False
                status = "newly_quiet" if hist_errors > 0 else "stable_quiet"
            else:
                still_open = True
                status = (
                    "regressed"
                    if hist_error_rate > 0 and recent_error_rate >= hist_error_rate
                    else "still_open"
                )

        return {
            "metric": metric,
            "status": status,
            "still_open": still_open,
            "weight": RECENCY_WEIGHTS.get(status, 0.5),
            "recent_calls": recent_calls,
            "recent_errors": recent_errors,
            "recent_error_rate": round(recent_error_rate, 4),
            "historical_calls": hist_calls,
            "historical_errors": hist_errors,
            "historical_error_rate": round(hist_error_rate, 4),
            "last_call_date": last_call_date,
            "has_recent_data": recent_calls > 0,
        }

    def _attach_recency(self, findings: List[Finding]) -> None:
        for finding in findings:
            if not finding.action_key:
                continue
            metric = "calls" if finding.category in CALLS_METRIC_CATEGORIES else "errors"
            recency = self.compute_recency(finding.action_key, metric)
            recency["recency_score"] = round(finding.score * recency["weight"], 3)
            finding.recency = recency

    def _finding_rank_score(self, finding: Finding) -> float:
        if self.args.rank_by_recency and finding.recency is not None:
            return float(finding.recency.get("recency_score", finding.score))
        return finding.score

    def build_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        findings.extend(self._build_noise_findings())
        findings.extend(self._build_maintenance_findings())
        findings.extend(self._build_schema_findings())
        findings.extend(self._build_unknown_action_findings())
        findings.extend(self._build_escape_hatch_findings())
        findings.extend(self._build_expected_slow_findings())
        findings.extend(self._build_child_process_findings())
        findings.extend(self._build_large_result_findings())
        findings.extend(self._build_high_error_findings())
        findings.extend(self._build_duplicate_retry_findings())
        findings.extend(self._build_slow_findings())
        findings.extend(self._build_mixed_schema_findings())
        findings.extend(self._build_environment_findings())
        findings.extend(self._build_parse_warning_findings())
        findings = [f for f in findings if severity_at_least(f.severity, self.args.min_severity)]
        if self.args.category:
            wanted = set(self.args.category)
            findings = [f for f in findings if f.category in wanted]
        self._attach_recency(findings)
        findings.sort(
            key=lambda f: (SEVERITY_RANK.get(f.severity, 0), self._finding_rank_score(f), f.confidence),
            reverse=True,
        )
        for rank, finding in enumerate(findings, 1):
            if finding.category in {"high_roi_candidate", "maintenance_loop", "schema_fix", "escape_hatch_replacement"}:
                finding.rank = rank
        return findings

    def _build_noise_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        heartbeat_count = self.noise_counts.get("heartbeat", 0)
        if heartbeat_count and not self.args.include_heartbeats:
            total = max(1, self.records_scanned)
            findings.append(
                Finding(
                    finding_id="noise_summary:heartbeat",
                    category="noise_summary",
                    severity="info",
                    confidence=1.0,
                    score=heartbeat_count,
                    title="Heartbeat/status records are excluded from default ROI ranking",
                    recommendation=(
                        "Keep heartbeat traffic visible in noise_summary, but do not let it dominate "
                        "high-ROI findings unless --include-heartbeats is passed."
                    ),
                    sample={
                        "heartbeat_records": heartbeat_count,
                        "percent_of_records": round(heartbeat_count * 100.0 / total, 2),
                    },
                )
            )
        synthetic_count = self.noise_counts.get("synthetic_test", 0)
        if synthetic_count and not self.args.include_synthetic_tests:
            findings.append(
                Finding(
                    finding_id="noise_summary:synthetic_test",
                    category="noise_summary",
                    severity="info",
                    confidence=1.0,
                    score=synthetic_count,
                    title="Synthetic test records are excluded from missing-action recommendations",
                    recommendation="Track synthetic rows separately so real caller confusion is not diluted.",
                    sample={"synthetic_test_records": synthetic_count},
                )
            )
        return findings

    def _build_maintenance_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for action_key, count in self.maintenance_count.most_common():
            total_ms = self.maintenance_duration.get(action_key, 0.0)
            if count < 3 or total_ms < SLOW_CALL_MS:
                continue
            score = total_ms / 1000.0 + count * 5.0
            severity = "high" if total_ms >= 600_000 or count >= 50 else "medium"
            findings.append(
                Finding(
                    finding_id="maintenance_loop:{0}".format(safe_id(action_key)),
                    category="maintenance_loop",
                    severity=severity,
                    confidence=0.88,
                    score=score,
                    title="{0} repeats expensive source/index maintenance".format(action_key),
                    recommendation=(
                        "Add freshness-gate evidence to the report and inspect whether this action is "
                        "being called without a stale-cache reason. Prefer repairing the trigger path "
                        "before optimizing the query itself."
                    ),
                    sample={
                        "count": count,
                        "total_duration_sec": round(total_ms / 1000.0, 3),
                        "avg_duration_ms": round(total_ms / count, 3),
                    },
                    action_key=action_key,
                    evidence=self.maintenance_evidence[action_key].items,
                )
            )
        return findings

    def _build_schema_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for group, count in self.schema_groups.most_common():
            if count < 2:
                continue
            action_key, detail = group.split("|", 1)
            severity = "high" if count >= 50 else "medium" if count >= 10 else "low"
            findings.append(
                Finding(
                    finding_id="schema_fix:{0}:{1}".format(safe_id(action_key), sha16(detail)),
                    category="schema_fix",
                    severity=severity,
                    confidence=0.9,
                    score=count * 20.0,
                    title="{0} has repeated schema confusion".format(action_key),
                    recommendation=(
                        "Inspect parameter names, aliases, schema examples, and action-discovery hints. "
                        "If callers consistently provide the same wrong key, consider a validated alias "
                        "or clearer schema text."
                    ),
                    sample={"count": count, "detail": detail},
                    action_key=action_key,
                    evidence=self.schema_evidence[group].items,
                )
            )
        return findings

    def _build_unknown_action_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for group, count in self.unknown_groups.most_common():
            if count < 2:
                continue
            severity = "high" if count >= 20 else "medium" if count >= 5 else "low"
            findings.append(
                Finding(
                    finding_id="needed_action:{0}".format(safe_id(group)),
                    category="needed_action",
                    severity=severity,
                    confidence=0.82,
                    score=count * 18.0,
                    title="{0} is repeatedly requested but unavailable".format(group),
                    recommendation=(
                        "Decide whether this should be a new action, a synonym/alias to an existing "
                        "action, or a clearer routing hint in discovery output."
                    ),
                    sample={"count": count},
                    action_key=group,
                    evidence=self.unknown_evidence[group].items,
                )
            )
        return findings

    def _build_escape_hatch_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for action_key, count in self.escape_hatch_count.most_common():
            if count < MIN_ESCAPE_HATCH_COUNT:
                continue
            error_count = self.error_by_action.get(action_key, 0)
            severity = "high" if count >= 100 else "medium"
            findings.append(
                Finding(
                    finding_id="escape_hatch_replacement:{0}".format(safe_id(action_key)),
                    category="escape_hatch_replacement",
                    severity=severity,
                    confidence=0.86,
                    score=count * 12.0 + error_count * 20.0,
                    title="{0} is used as an escape hatch".format(action_key),
                    recommendation=(
                        "Cluster bounded arguments/results for this escape hatch and promote repeated "
                        "patterns into first-class Monolith actions or stronger discovery guidance."
                    ),
                    sample={"count": count, "errors": error_count},
                    action_key=action_key,
                    evidence=self.escape_hatch_evidence[action_key].items,
                )
            )
        return findings

    def _build_expected_slow_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for action_key, count in self.expected_slow_count.most_common():
            errors = self.expected_slow_errors.get(action_key, 0)
            if count < 5 or errors == 0:
                continue
            total_ms = self.expected_slow_duration.get(action_key, 0.0)
            error_rate = errors / count
            severity = "high" if error_rate >= 0.35 and count >= 20 else "medium"
            findings.append(
                Finding(
                    finding_id="expected_slow_domain_error:{0}".format(safe_id(action_key)),
                    category="expected_slow_domain_error",
                    severity=severity,
                    confidence=0.84,
                    score=errors * 25.0 + error_rate * 100.0,
                    title="{0} is slow by domain but also has a high error burden".format(action_key),
                    recommendation=(
                        "Rank this domain primarily by error/retry rate, not raw duration. Improve "
                        "preflight validation and actionable error messages before optimizing latency."
                    ),
                    sample={
                        "count": count,
                        "errors": errors,
                        "error_rate": round(error_rate, 3),
                        "total_duration_sec": round(total_ms / 1000.0, 3),
                    },
                    action_key=action_key,
                    evidence=self.expected_slow_evidence[action_key].items,
                )
            )
        return findings

    def _build_child_process_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for action_key, count in self.child_process_count.most_common():
            total_ms = self.child_process_duration.get(action_key, 0.0)
            if count == 0:
                continue
            parent_stats = self.duration_by_action[action_key].as_row()
            parent_total_ms = parent_stats.get("total_ms", 0.0)
            dominance = total_ms / parent_total_ms if parent_total_ms else 0.0
            severity = "medium" if total_ms >= SLOW_CALL_MS or dominance >= 0.7 else "low"
            findings.append(
                Finding(
                    finding_id="child_query_bottleneck:{0}".format(safe_id(action_key)),
                    category="child_query_bottleneck",
                    severity=severity,
                    confidence=0.82,
                    score=total_ms / 1000.0 + count * 10.0,
                    title="{0} launches child query processes".format(action_key),
                    recommendation=(
                        "Attribute child query time to the parent action in ROI tables and avoid "
                        "counting parent/child rows as unrelated slow calls. If this is frequent, "
                        "consider direct in-process data access or clearer freshness gating."
                    ),
                    sample={
                        "count": count,
                        "child_duration_sec": round(total_ms / 1000.0, 3),
                        "parent_duration_sec": round(parent_total_ms / 1000.0, 3),
                        "child_parent_ratio": round(dominance, 3),
                    },
                    action_key=action_key,
                    evidence=self.child_process_evidence[action_key].items,
                )
            )
        return findings

    def _build_large_result_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for action_key, (payload, evidence) in sorted(
            self.max_payload_by_action.items(), key=lambda item: item[1][0], reverse=True
        ):
            if payload < LARGE_RESULT_BYTES:
                continue
            findings.append(
                Finding(
                    finding_id="large_result:{0}".format(safe_id(action_key)),
                    category="large_result",
                    severity="medium" if payload >= 500_000 else "low",
                    confidence=0.8,
                    score=payload / 1000.0,
                    title="{0} can emit large result payloads".format(action_key),
                    recommendation=(
                        "Inspect projection, pagination, duplicate row suppression, and default limit "
                        "behavior for this action."
                    ),
                    sample={"max_payload_bytes": payload, "total_payload_bytes": self.payload_by_action[action_key]},
                    action_key=action_key,
                    evidence=[evidence],
                )
            )
        return findings[: self.args.top]

    def _build_high_error_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for action_key, count in self.finding_count_by_action.most_common():
            if count < MIN_ERROR_RATE_COUNT:
                continue
            errors = self.finding_error_by_action.get(action_key, 0)
            if errors == 0:
                continue
            rate = errors / count
            if rate < 0.25:
                continue
            severity = "high" if rate >= 0.75 else "medium" if rate >= 0.4 else "low"
            findings.append(
                Finding(
                    finding_id="high_error_rate:{0}".format(safe_id(action_key)),
                    category="high_error_rate",
                    severity=severity,
                    confidence=0.78,
                    score=errors * 10.0 + rate * 100.0,
                    title="{0} has a high error rate".format(action_key),
                    recommendation=(
                        "Inspect this action before adding adjacent new surfaces. High error-rate "
                        "actions should become self-correcting or return stronger hints."
                    ),
                    sample={"count": count, "errors": errors, "error_rate": round(rate, 3)},
                    action_key=action_key,
                    evidence=self.finding_error_evidence[action_key].items,
                )
            )
        return findings

    def _build_duplicate_retry_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for (action_key, retry_signature), count in self.retry_counts.most_common():
            if count < MIN_DUPLICATE_RETRY_COUNT:
                continue
            noise_class = dominant_noise_class(action_key, self.noise_by_action)
            if noise_class == "heartbeat" and not self.args.include_heartbeats:
                continue
            if noise_class == "synthetic_test" and not self.args.include_synthetic_tests:
                continue
            severity = "medium" if count >= 20 else "low"
            findings.append(
                Finding(
                    finding_id="duplicate_retry:{0}:{1}".format(safe_id(action_key), sha16(retry_signature)),
                    category="duplicate_retry",
                    severity=severity,
                    confidence=0.74,
                    score=count * 4.0,
                    title="{0} repeats the same retry signature".format(action_key),
                    recommendation=(
                        "Check whether the caller is polling intentionally, retrying without new evidence, "
                        "or missing a cached/freshness-aware action."
                    ),
                    sample={"count": count, "retry_signature": retry_signature},
                    action_key=action_key,
                    evidence=self.retry_evidence[(action_key, retry_signature)].items,
                )
            )
        return findings[: self.args.top]

    def _build_slow_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for action_key, stats in self.duration_by_action.items():
            row = stats.as_row()
            p95 = row["p95_ms"]
            if stats.count < 5 or p95 < SLOW_CALL_MS:
                continue
            noise_class = dominant_noise_class(action_key, self.noise_by_action)
            if noise_class in {"maintenance", "expected_slow_domain"}:
                continue
            severity = "high" if p95 >= VERY_SLOW_CALL_MS else "medium"
            findings.append(
                Finding(
                    finding_id="slow_action:{0}".format(safe_id(action_key)),
                    category="slow_action",
                    severity=severity,
                    confidence=0.72,
                    score=p95 / 1000.0 + stats.count,
                    title="{0} has slow p95 latency".format(action_key),
                    recommendation="Inspect phase timing and result shape before treating this as a code hot path.",
                    sample=row,
                    action_key=action_key,
                    evidence=[item for item in self.slow_calls.items_desc() if item.surface + ":" + item.namespace + "." + item.action == action_key][:DEFAULT_EVIDENCE_LIMIT],
                )
            )
        return findings

    def _build_mixed_schema_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for day, versions in sorted(self.mixed_schema_days.items()):
            present = [version for version, count in versions.items() if count]
            if len(present) <= 1:
                continue
            findings.append(
                Finding(
                    finding_id="mixed_schema:{0}".format(day),
                    category="mixed_schema",
                    severity="info",
                    confidence=1.0,
                    score=sum(versions.values()),
                    title="{0} contains mixed log format versions".format(day),
                    recommendation="Keep per-record format dispatch. Do not treat date folders as one schema.",
                    sample={"versions": dict(sorted(versions.items()))},
                )
            )
        return findings

    def _build_environment_findings(self) -> List[Finding]:
        findings: List[Finding] = []
        for key, bucket in sorted(self.environment_issue_evidence.items()):
            _prefix, value = key.split(":", 1)
            count = self.index_health_counts.get(value, 0)
            severity = "medium" if value in {"missing", "stale", "error", "unhealthy"} else "low"
            findings.append(
                Finding(
                    finding_id="index_health:{0}".format(safe_id(value)),
                    category="index_health",
                    severity=severity,
                    confidence=0.84,
                    score=count * 8.0,
                    title="environment.index_health reports {0}".format(value),
                    recommendation=(
                        "Separate runtime/index blockers from action-design findings. If this repeats "
                        "near source/project calls, repair or refresh the matching index before changing "
                        "action contracts."
                    ),
                    sample={"index_health": value, "count": count},
                    evidence=bucket.items,
                )
            )
        return findings

    def _build_parse_warning_findings(self) -> List[Finding]:
        if not self.parse_warnings:
            return []
        return [
            Finding(
                finding_id="parse_warnings",
                category="parse_warnings",
                severity="medium" if self.args.strict else "low",
                confidence=1.0,
                score=len(self.parse_warnings),
                title="Some log lines could not be parsed cleanly",
                recommendation="Inspect parse_warnings.csv; strict mode exits on these rows.",
                sample={"warnings": len(self.parse_warnings)},
            )
        ]

    def summary_json(self, files: Sequence[Path], findings: Sequence[Finding]) -> Dict[str, Any]:
        return {
            "schema_version": FINDINGS_SCHEMA_VERSION,
            "generated_at": self.generated_at,
            "inputs": {
                "log_roots": [str(Path(item)) for item in self.args.log_root],
                "since": self.args.since,
                "until": self.args.until,
                "files": list(self.args.files),
                "max_lines": self.args.max_lines,
                "include_heartbeats": self.args.include_heartbeats,
                "include_synthetic_tests": self.args.include_synthetic_tests,
                "recent_days": self.args.recent_days,
                "fix_boundary": self.args.fix_boundary,
                "rank_by_recency": self.args.rank_by_recency,
            },
            "recency": self.recency_views(findings),
            "summary": {
                "records_scanned": self.records_scanned,
                "files_scanned": self.files_scanned,
                "candidate_files": len(files),
                "parse_warnings": len(self.parse_warnings),
                "counts_by_surface_status": counter_to_nested(self.count_by_surface_status),
                "counts_by_surface_version": counter_to_nested(self.count_by_surface_version),
                "noise_counts": dict(self.noise_counts.most_common()),
                "index_health_counts": dict(self.index_health_counts.most_common()),
                "headless_counts": dict(self.headless_counts.most_common()),
                "profile_counts": dict(self.profile_counts.most_common()),
                "findings_by_severity": dict(Counter(f.severity for f in findings)),
                "findings_by_category": dict(Counter(f.category for f in findings)),
                "escape_hatch_clusters": self.escape_hatch_cluster_rows()[: self.args.top],
            },
            "findings": [finding.to_json() for finding in findings],
        }

    def action_stats_rows(self) -> List[Dict[str, Any]]:
        rows: List[Dict[str, Any]] = []
        for action_key, count in self.count_by_action.most_common():
            stats = self.duration_by_action[action_key].as_row()
            noise_class = dominant_noise_class(action_key, self.noise_by_action)
            rows.append(
                {
                    "action_key": action_key,
                    "count": count,
                    "errors": self.error_by_action.get(action_key, 0),
                    "error_rate": round(self.error_by_action.get(action_key, 0) / count, 4) if count else 0.0,
                    "noise_class": noise_class,
                    "payload_bytes_total": self.payload_by_action.get(action_key, 0),
                    **stats,
                }
            )
        return rows

    def duplicate_rows(self) -> List[Dict[str, Any]]:
        rows: List[Dict[str, Any]] = []
        for (action_key, retry_signature), count in self.retry_counts.most_common():
            if count < 2:
                continue
            noise_class = dominant_noise_class(action_key, self.noise_by_action)
            rows.append(
                {
                    "action_key": action_key,
                    "retry_signature": retry_signature,
                    "count": count,
                    "noise_class": noise_class,
                }
            )
        return rows[: max(self.args.top, 100)]

    def escape_hatch_cluster_rows(self) -> List[Dict[str, Any]]:
        rows: List[Dict[str, Any]] = []
        for (action_key, cluster), count in self.escape_hatch_clusters.most_common():
            errors = self.escape_hatch_cluster_errors.get((action_key, cluster), 0)
            evidence = self.escape_hatch_cluster_evidence[(action_key, cluster)].items
            first = evidence[0] if evidence else None
            rows.append(
                {
                    "action_key": action_key,
                    "cluster": cluster,
                    "count": count,
                    "errors": errors,
                    "error_rate": round(errors / count, 4) if count else 0.0,
                    "recommended_action": escape_hatch_recommendation(cluster),
                    "first_source_file": first.source_file if first else "",
                    "first_line_number": first.line_number if first else "",
                }
            )
        return rows

    def recency_views(self, findings: Sequence[Finding]) -> Dict[str, Any]:
        """Recency-split views (6B): still-open, regressions, newly-quiet, no-data.

        Findings arrive already sorted by build_findings, so each list is in rank
        order. no_recent_data is kept distinct from quiet so an editor action with
        0 recent calls is never silently read as 'passing' (spec §6B item 4).
        """
        def row(finding: Finding) -> Dict[str, Any]:
            rec = finding.recency or {}
            return {
                "finding_id": finding.finding_id,
                "category": finding.category,
                "action_key": finding.action_key,
                "severity": finding.severity,
                "score": round(finding.score, 3),
                "recency_score": rec.get("recency_score", round(finding.score, 3)),
                "still_open": rec.get("still_open"),
                "status": rec.get("status"),
                "metric": rec.get("metric"),
                "recent_calls": rec.get("recent_calls"),
                "recent_errors": rec.get("recent_errors"),
                "historical_errors": rec.get("historical_errors"),
                "last_call_date": rec.get("last_call_date"),
            }

        scored = [finding for finding in findings if finding.recency is not None]
        top = self.args.top
        return {
            "recent_days": self.args.recent_days,
            "fix_boundary": self.args.fix_boundary,
            "rank_by_recency": self.args.rank_by_recency,
            "recent_dates": sorted(self._recent_date_set()),
            "still_open": [row(f) for f in scored if f.recency.get("still_open") is True][:top],
            "regressions": [row(f) for f in scored if f.recency.get("status") == "regressed"][:top],
            "newly_quiet": [row(f) for f in scored if f.recency.get("status") == "newly_quiet"][:top],
            "no_recent_data": [row(f) for f in scored if f.recency.get("status") == "no_recent_data"][:top],
        }


class StrictParseError(Exception):
    def __init__(self, path: Path, line_number: int, original: BaseException) -> None:
        super().__init__("{0}:{1}: {2}".format(path, line_number, original))
        self.path = path
        self.line_number = line_number
        self.original = original


def normalize_event(raw: Dict[str, Any], path: Path, line_number: int, repo_root: Path, include_paths: bool) -> CanonicalEvent:
    call = raw.get("call") if isinstance(raw.get("call"), dict) else {}
    agent_signal = raw.get("agent_signal") if isinstance(raw.get("agent_signal"), dict) else {}
    return_summary = raw.get("return_summary") if isinstance(raw.get("return_summary"), dict) else {}
    phase_timing = raw.get("phase_timing") if isinstance(raw.get("phase_timing"), dict) else {}
    environment = raw.get("environment") if isinstance(raw.get("environment"), dict) else {}
    child_process = raw.get("child_process") if isinstance(raw.get("child_process"), dict) else {}

    surface = str(raw.get("surface") or path.stem or "unknown")
    format_version = coerce_int(raw.get("format_version"), 0)
    namespace, action, tool_name = extract_call_identity(call, surface)
    action_key = "{0}:{1}.{2}".format(surface, namespace, action)

    duration_ms = coerce_float(raw.get("duration_ms"))
    time_since_previous_ms = coerce_float(raw.get("time_since_previous_ms"))
    status = str(raw.get("status") or "unknown")
    retry_signature = str(
        agent_signal.get("retry_signature")
        or get_nested(raw, "workflow", "retry_signature")
        or call.get("retry_signature")
        or ""
    )
    argument_fingerprint = build_argument_fingerprint(call)
    record_key = str(raw.get("record_id") or "")
    if not record_key:
        record_key = "local:{0}".format(
            sha16("{0}|{1}|{2}|{3}|{4}".format(path, line_number, action_key, raw.get("start_time", ""), retry_signature))
        )

    outcome, error_class, error_code, message = extract_error_shape(raw, agent_signal)
    payload_bytes = extract_payload_bytes(raw, agent_signal, return_summary)
    tags = tuple(str(tag) for tag in agent_signal.get("improvement_tags") or [])
    noise_class = classify_noise(surface, namespace, action, tool_name, tags, call, message, environment)
    escape_cluster = escape_hatch_cluster(call) if noise_class == "escape_hatch" else ""
    parse_warnings = build_event_warnings(raw, namespace, action)
    source_file = shorten_path(path, repo_root, include_paths)

    return CanonicalEvent(
        source_path=path,
        source_file=source_file,
        line_number=line_number,
        surface=surface,
        format_version=format_version,
        record_key=record_key,
        trace_id=str(raw.get("trace_id") or ""),
        span_id=str(raw.get("span_id") or ""),
        parent_span_id=str(raw.get("parent_span_id") or ""),
        previous_record_id=str(raw.get("previous_record_id") or ""),
        process_instance_id=str(raw.get("process_instance_id") or ""),
        session_key=str(raw.get("session_key") or ""),
        start_time=str(raw.get("start_time") or ""),
        end_time=str(raw.get("end_time") or ""),
        duration_ms=duration_ms,
        time_since_previous_ms=time_since_previous_ms,
        status=status,
        namespace=namespace,
        action=action,
        tool_name=tool_name,
        action_key=action_key,
        argument_fingerprint=argument_fingerprint,
        retry_signature=retry_signature,
        return_summary=return_summary,
        agent_signal=agent_signal,
        phase_timing=phase_timing,
        environment=environment,
        child_process=child_process,
        outcome=outcome,
        error_class=error_class,
        error_code=error_code,
        message=message,
        payload_bytes=payload_bytes,
        tags=tags,
        noise_class=noise_class,
        escape_hatch_cluster=escape_cluster,
        parse_warnings=tuple(parse_warnings),
    )


def extract_call_identity(call: Dict[str, Any], surface: str) -> Tuple[str, str, str]:
    namespace = str(call.get("namespace") or call.get("namespace_name") or "")
    action = str(call.get("action") or call.get("action_name") or "")
    tool_name = str(call.get("tool_name") or call.get("tool_name_original") or call.get("tool_name_forwarded") or "")

    if not action and tool_name:
        action = tool_name
    if not namespace and tool_name.startswith("monolith_"):
        namespace = "mcp"
    if not namespace:
        namespace = "<none>"
    if not action:
        action = "<none>"
    if not tool_name:
        tool_name = "{0}.{1}".format(namespace, action)
    return namespace, action, tool_name


def build_argument_fingerprint(call: Dict[str, Any]) -> str:
    relevant = {
        "arguments": call.get("arguments"),
        "options": call.get("options"),
        "positional": call.get("positional"),
        "argv": call.get("argv"),
    }
    return "sha256:{0}".format(sha16(stable_json(relevant)))


def extract_error_shape(raw: Dict[str, Any], agent_signal: Dict[str, Any]) -> Tuple[str, str, str, str]:
    ret = raw.get("return") if isinstance(raw.get("return"), dict) else {}
    outcome = str(agent_signal.get("outcome") or "")
    error_class = str(agent_signal.get("error_class") or ret.get("error_class") or "")
    error_code = str(agent_signal.get("error_code") or ret.get("error_code") or "")
    message = ret.get("error_message") or ret.get("fatal_error") or ""
    error_obj = ret.get("error")
    if isinstance(error_obj, dict):
        if not message:
            message = error_obj.get("message") or ""
        if not error_code:
            error_code = str(error_obj.get("code") or "")
    if not message and isinstance(ret.get("stderr"), str):
        message = ret.get("stderr") or ""
    return outcome, error_class, error_code, compact_message(message)


def extract_payload_bytes(raw: Dict[str, Any], agent_signal: Dict[str, Any], return_summary: Dict[str, Any]) -> int:
    for data in (return_summary, agent_signal, raw.get("return") if isinstance(raw.get("return"), dict) else {}):
        for key in (
            "payload_bytes",
            "result_bytes",
            "response_bytes",
            "stdout_bytes",
            "stderr_bytes",
            "bytes",
            "argument_bytes",
        ):
            value = data.get(key)
            if isinstance(value, (int, float)) and value > 0:
                return int(value)
    return 0


def build_event_warnings(raw: Dict[str, Any], namespace: str, action: str) -> List[str]:
    warnings: List[str] = []
    if not raw.get("format_version"):
        warnings.append("missing_format_version")
    if namespace == "<none>" or action == "<none>":
        warnings.append("degraded_call_identity")
    return warnings


def classify_noise(
    surface: str,
    namespace: str,
    action: str,
    tool_name: str,
    tags: Sequence[str],
    call: Optional[Dict[str, Any]] = None,
    message: str = "",
    environment: Optional[Dict[str, Any]] = None,
) -> str:
    # v3 logger environment stamp is the primary synthetic signal. Marker and
    # per-action fixture whitelists below stay only as fallback for legacy rows
    # written before environment.is_automation_test existed.
    if environment and environment.get("is_automation_test") is True:
        return "synthetic_test"
    ns_action = "{0}.{1}".format(namespace, action).lower()
    tool_lower = tool_name.lower()
    if ns_action in {"monolith.status", "mcp.monolith_status"} or action.lower() == "monolith_status":
        return "heartbeat"
    if ns_action in SYNTHETIC_ACTION_IDS:
        return "synthetic_test"
    if "__missing_action" in ns_action or "__cc05" in ns_action or namespace.startswith("__") or action.startswith("__"):
        return "synthetic_test"
    if is_synthetic_param_guard_fixture(namespace, action, call or {}, message):
        return "synthetic_test"
    if namespace == "source" and action in {
        "repair_crg_cache",
        "build_crg_graph",
        "health",
        "crg_graph_health",
        "repair_fts",
        "trigger_project_reindex",
    }:
        return "maintenance"
    if namespace == "index" and action in {"get_index_status", "trigger_project_reindex", "repair_crg_cache"}:
        return "maintenance"
    if namespace == "imagegen" and (
        action.startswith("generate_image")
        or action in {"generate_image", "generate_image_via_ima2", "generate_svg", "generate_msdf_from_svg"}
    ):
        return "expected_slow_domain"
    if action == "run_python" and namespace == "editor":
        return "escape_hatch"
    if "escape_hatch" in tags or "escape_hatch" in tool_lower:
        return "escape_hatch"
    return "normal"


ESCAPE_HATCH_PATTERNS: Tuple[Tuple[str, Tuple[str, ...]], ...] = (
    ("editor_asset_library", ("editorassetlibrary", "editor_asset_library", "save_loaded_asset", "duplicate_asset", "delete_asset")),
    ("asset_registry", ("assetregistry", "asset_registry", "get_assets_by_path", "get_asset_by_object_path")),
    ("widget_blueprint", ("widgetblueprint", "widget_blueprint", "widgettree", "widget_tree", "uwidget")),
    ("blueprint", ("blueprint", "k2node", "simpleconstructionscript", "compile_blueprint")),
    ("level_actor", ("level_actor", "get_selected_level_actors", "get_all_level_actors", "spawn_actor", "set_actor", "editorlevel", "editor_level")),
    ("subsystem", ("get_editor_subsystem", "get_engine_subsystem", "subsystem")),
    ("material", ("material", "materialeditinglibrary", "material_editing_library")),
    ("asset_load", ("load_asset", "staticloadobject", "loadobject", "find_object", "softobjectpath")),
    ("python_print_only", ("print(", "unreal.log", "log_warning")),
)


def escape_hatch_cluster(call: Dict[str, Any]) -> str:
    args = call.get("arguments") if isinstance(call.get("arguments"), dict) else {}
    script_parts: List[str] = []
    for key in ("code", "command", "script", "python"):
        value = args.get(key)
        if isinstance(value, str):
            script_parts.append(value[:4000])
    if not script_parts and args:
        script_parts.append(stable_json(args)[:4000])
    if not script_parts:
        return "no_script"

    script = "\n".join(script_parts).lower()
    for cluster, needles in ESCAPE_HATCH_PATTERNS:
        if any(needle in script for needle in needles):
            return cluster
    return "other"


def escape_hatch_recommendation(cluster: str) -> str:
    recommendations = {
        "editor_asset_library": "Prefer asset.* or project.* actions; add a first-class editor asset action for repeated gaps.",
        "asset_registry": "Prefer project.search/project.get_asset_details; add indexed query coverage if provenance is missing.",
        "widget_blueprint": "Prefer ui.* actions; promote repeated WidgetTree edits into UI actions.",
        "blueprint": "Prefer blueprint.* actions; add missing graph/component/CDO verbs for repeated scripts.",
        "level_actor": "Prefer scene.* or editor selection/level actions; add a scene action if the pattern repeats.",
        "subsystem": "Expose the repeated subsystem call as a typed Monolith action.",
        "material": "Prefer material.* actions; add graph/property verbs for repeated scripts.",
        "asset_load": "Prefer project/asset lookup actions before loading assets through Python.",
        "python_print_only": "Usually diagnostic noise; keep out of action-gap prioritization unless repeated with errors.",
        "no_script": "Schema issue: accept code/script/command aliases or return focused guidance.",
    }
    return recommendations.get(cluster, "Cluster manually; promote repeated bounded scripts into first-class actions or discovery hints.")


def is_synthetic_param_guard_fixture(
    namespace: str,
    action: str,
    call: Dict[str, Any],
    message: str,
) -> bool:
    args = call.get("arguments")
    args_blob = stable_json(args).lower() if args is not None else ""
    message_lower = (message or "").lower()

    if any(marker in args_blob or marker in message_lower for marker in SYNTHETIC_ARGUMENT_MARKERS):
        return True

    if namespace == "gas":
        if action == "scaffold_custom_ability_task" and isinstance(args, dict):
            return args.get("class_name") == "MyCustomTask"
        if action == "scaffold_status_effect" and isinstance(args, dict):
            return args.get("save_path") == "/Game/Effects/GE_Test" and args.get("name") == "TestEffect"
        if action == "scaffold_weapon_ability" and isinstance(args, dict):
            return args.get("save_path") == "/Game/Abilities/GA_TestWeapon" and args.get("weapon_type") == "pistol"
        if action == "create_target_actor" and isinstance(args, dict):
            return args.get("save_path") == "/Game/GAS/Targeting/TA_Test" and args.get("targeting_type") == "line"
        if action == "configure_target_actor" and isinstance(args, dict):
            return args.get("asset_path") == "/Game/GAS/Targeting/TA_Test"
        if action == "scaffold_fps_targeting" and isinstance(args, dict):
            return args.get("ability_path") == "/Game/GAS/Abilities/GA_Test" and args.get("mode") == "hitscan"

    if namespace == "input" and action == "create_input_action" and isinstance(args, dict):
        return args.get("asset_path") == "/Game/GAS/Input/IA_ParamGuard"

    if namespace == "worldgen" and action == "scatter_props" and isinstance(args, dict):
        volume_name = str(args.get("volume_name") or "")
        asset_paths = args.get("asset_paths")
        count = args.get("count")
        collision_mode = str(args.get("collision_mode") or "")
        has_test_prop = isinstance(asset_paths, list) and asset_paths == ["/Game/TestProp"]
        if not args and "missing required param" in message_lower and "volume_name" in message_lower:
            return True
        if volume_name == "TestVolume" and (
            "missing required param" in message_lower
            or (has_test_prop and count == 0)
            or (has_test_prop and count == 5 and collision_mode == "invalid_mode")
        ):
            return True

    if namespace == "material" and action == "create_function_instance" and isinstance(args, dict):
        if args.get("parent") == "/Game/ParentFunction":
            return "malformedpath" in args_blob or "testfunctioninstance" in args_blob or "package path is empty" in message_lower

    if namespace == "material" and action == "move_expression" and isinstance(args, dict):
        if args.get("asset_path") == "/Game/DoesNotMatter":
            if args.get("pos_x") is True or args.get("expression_name") == 123:
                return True
            expressions = args.get("expressions")
            if isinstance(expressions, list) and len(expressions) == 1 and isinstance(expressions[0], dict):
                expr = expressions[0]
                if expr == {"x": 100}:
                    return True
                if expr.get("name") == "MyExpr" and expr.get("x") == "100":
                    return True

    if namespace == "imagegen" and isinstance(args, dict):
        if action == "generate_svg":
            prompt = str(args.get("prompt") or "").lower()
            if "deterministic msdf-ready diamond icon" in prompt:
                return True

        if action == "generate_image_via_ima2":
            prompt = str(args.get("prompt") or "").lower()
            server_url = str(args.get("server_url") or "").lower()
            texture_role = str(args.get("texture_role") or "").lower()
            output_format = str(args.get("format") or "").lower()
            background = str(args.get("background") or "").lower()
            if "validation smoke" in prompt or "reference input archive smoke" in prompt or "reference extraction only" in prompt:
                return True
            if "127.0.0.1:9" in server_url and (
                str(args.get("timeout_seconds") or "") == "1"
                or args.get("save") is False
                or "/game/tests/monolith" in args_blob
            ):
                return True
            if texture_role == "mystery_role":
                return True
            if output_format in {"webp", "jpeg", "jpg"} and "unsupported" in prompt:
                return True
            if texture_role == "normal" and background == "transparent" and "normal map transparent" in prompt:
                return True

        if action == "validate_svg":
            svg_text = str(args.get("svg_text") or "").lower()
            file_path = str(args.get("file_path") or "").replace("\\", "/").lower()
            if any(
                marker in svg_text
                for marker in (
                    "<script>alert(1)</script>",
                    "onload=\"alert(1)\"",
                    "<foreignobject>",
                    "<!doctype svg",
                    "example.invalid",
                    "<path d=\"m 0 0 l 10\"",
                )
            ):
                return True
            if file_path.endswith("/generatedimages/vector/v_roundtripsvg.svg"):
                return True

    return False


def is_schema_confusion(event: CanonicalEvent) -> bool:
    if "schema_confusing" in event.tags:
        return True
    if event.outcome == "validation_rejected":
        return True
    if event.error_class in {"missing_param", "invalid_param", "schema_error", "validation_error"}:
        return True
    lowered = event.message.lower()
    return "missing required param" in lowered or "requires a" in lowered or "provided keys" in lowered


def schema_group_key(event: CanonicalEvent) -> str:
    detail = event.message or event.error_class or event.outcome or "schema_confusion"
    missing = extract_missing_param_detail(detail)
    if missing:
        detail = missing
    return "{0}|{1}".format(event.action_key, compact_message(detail, 180))


def extract_missing_param_detail(message: str) -> str:
    match = re.search(r"Missing required param\(s\):\s*\[([^\]]*)\]\.\s*Provided keys:\s*\[([^\]]*)\]", message)
    if match:
        missing = match.group(1).strip()
        provided = match.group(2).strip()
        return "missing=[{0}] provided=[{1}]".format(missing, provided)
    match = re.search(r"([A-Za-z0-9_]+) requires (?:a|an) ([A-Za-z0-9_]+)", message)
    if match:
        return "{0} requires {1}".format(match.group(1), match.group(2))
    return ""


def is_unknown_action(event: CanonicalEvent) -> bool:
    if "missing_action" in event.tags:
        return True
    if event.outcome == "unknown_action" or event.error_class == "unknown_action":
        return True
    return "unknown action" in event.message.lower()


def unknown_group_key(event: CanonicalEvent) -> str:
    if event.namespace != "<none>" and event.action != "<none>":
        return event.action_key
    match = re.search(r"Unknown (?:[A-Za-z0-9_]+ )?action:\s*([A-Za-z0-9_.\-]+)", event.message)
    if match:
        return "{0}:{1}".format(event.surface, match.group(1))
    return event.action_key


def child_process_duration(child_process: Dict[str, Any]) -> Optional[float]:
    if not child_process:
        return None
    return first_number(child_process, ("exec_process_ms", "duration_ms", "process_ms"))


def dominant_noise_class(action_key: str, counter: Counter[Tuple[str, str]]) -> str:
    rows = [(count, noise) for (noise, key), count in counter.items() if key == action_key]
    if not rows:
        return "normal"
    rows.sort(reverse=True)
    return rows[0][1]


def safe_id(text: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.:-]+", "_", text).strip("_")
    return value[:120] or "unknown"


def counter_to_nested(counter: Counter[Tuple[Any, Any]]) -> Dict[str, Dict[str, int]]:
    result: Dict[str, Dict[str, int]] = {}
    for (left, right), count in counter.items():
        result.setdefault(str(left), {})[str(right)] = count
    return result


def discover_log_files(log_roots: Sequence[Path], surfaces: Sequence[str], since: Optional[str], until: Optional[str]) -> List[Path]:
    wanted = {surface.lower() for surface in surfaces}
    files: List[Path] = []
    for root in log_roots:
        root = root.resolve()
        if root.is_file() and root.suffix.lower() == ".jsonl":
            if root.stem.lower() in wanted:
                files.append(root)
            continue
        if not root.exists():
            continue
        direct_jsonl = [p for p in root.glob("*.jsonl") if p.stem.lower() in wanted]
        files.extend(sorted(direct_jsonl, key=lambda p: (p.stem, str(p))))
        for child in sorted(root.iterdir(), key=lambda p: p.name):
            if not child.is_dir() or not re.fullmatch(r"\d{8}", child.name):
                continue
            if since and child.name < since:
                continue
            if until and child.name > until:
                continue
            for surface in SURFACE_FILES:
                if surface not in wanted:
                    continue
                path = child / "{0}.jsonl".format(surface)
                if path.exists():
                    files.append(path)
    # Stable de-duplication preserving order.
    seen = set()
    unique: List[Path] = []
    for path in files:
        key = str(path.resolve()).lower()
        if key in seen:
            continue
        seen.add(key)
        unique.append(path)
    return unique


def ensure_output_dir(out_dir: Path, log_roots: Sequence[Path]) -> None:
    for root in log_roots:
        if root.exists() and path_is_relative_to(out_dir, root):
            raise ValueError("output directory must not be under log root: {0}".format(out_dir))
    out_dir.mkdir(parents=True, exist_ok=True)


def write_json_report(path: Path, data: Dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def write_csv(path: Path, rows: Sequence[Dict[str, Any]]) -> None:
    keys: List[str] = []
    for row in rows:
        for key in row.keys():
            if key not in keys:
                keys.append(key)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=keys or ["empty"])
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_reports(analyzer: Analyzer, files: Sequence[Path], findings: Sequence[Finding]) -> None:
    out = analyzer.args.out
    summary_data = analyzer.summary_json(files, findings)
    formats = set(analyzer.args.format)
    if "json" in formats:
        write_json_report(out / "findings.json", summary_data)
    if "csv" in formats:
        write_csv(out / "action_stats.csv", analyzer.action_stats_rows())
        write_csv(out / "slow_calls.csv", [item.to_json() for item in analyzer.slow_calls.items_desc()])
        write_csv(out / "duplicates.csv", analyzer.duplicate_rows())
        write_csv(out / "escape_hatch_clusters.csv", analyzer.escape_hatch_cluster_rows())
        write_csv(out / "parse_warnings.csv", analyzer.parse_warnings)
    if "markdown" in formats:
        (out / "summary.md").write_text(render_markdown_summary(analyzer, findings), encoding="utf-8", newline="\n")


def render_markdown_summary(analyzer: Analyzer, findings: Sequence[Finding]) -> str:
    lines: List[str] = []
    lines.append("# Monolith Invocation Log Analysis")
    lines.append("")
    lines.append("- Generated: `{0}`".format(analyzer.generated_at))
    lines.append("- Records scanned: `{0}`".format(analyzer.records_scanned))
    lines.append("- Files scanned: `{0}`".format(analyzer.files_scanned))
    lines.append("- Parse warnings: `{0}`".format(len(analyzer.parse_warnings)))
    lines.append("- Output: `{0}`".format(analyzer.args.out))
    if analyzer.all_date_keys:
        recent = sorted(analyzer._recent_date_set())
        window = (
            "fix_boundary >= {0}".format(analyzer.args.fix_boundary)
            if analyzer.args.fix_boundary
            else "last {0} present day(s)".format(analyzer.args.recent_days)
        )
        lines.append(
            "- Recency window: `{0}` -> `{1}`{2}".format(
                window,
                ", ".join(recent) if recent else "(none)",
                " (rank-by-recency)" if analyzer.args.rank_by_recency else "",
            )
        )
    lines.append("")

    lines.append("## Surface Status")
    lines.append("")
    lines.append("| Surface | Status | Count |")
    lines.append("|---|---:|---:|")
    for (surface, status), count in sorted(analyzer.count_by_surface_status.items()):
        lines.append("| `{0}` | `{1}` | {2} |".format(surface, status, count))
    lines.append("")

    lines.append("## Noise Summary")
    lines.append("")

    if analyzer.index_health_counts:
        lines.append("## Environment Summary")
        lines.append("")
        lines.append("| Field | Value | Count |")
        lines.append("|---|---|---:|")
        for value, count in analyzer.index_health_counts.most_common():
            lines.append("| `index_health` | `{0}` | {1} |".format(value, count))
        for value, count in analyzer.headless_counts.most_common():
            lines.append("| `headless` | `{0}` | {1} |".format(value, count))
        for value, count in analyzer.profile_counts.most_common():
            lines.append("| `active_profile_id` | `{0}` | {1} |".format(value, count))
        lines.append("")
    lines.append("| Noise class | Count |")
    lines.append("|---|---:|")
    for noise, count in analyzer.noise_counts.most_common():
        lines.append("| `{0}` | {1} |".format(noise, count))
    lines.append("")

    escape_rows = analyzer.escape_hatch_cluster_rows()
    if escape_rows:
        lines.append("## Escape Hatch Clusters")
        lines.append("")
        lines.append("| Action | Cluster | Count | Errors | Recommended action | First evidence |")
        lines.append("|---|---|---:|---:|---|---|")
        for row in escape_rows[: analyzer.args.top]:
            first_evidence = ""
            if row["first_source_file"]:
                first_evidence = "`{0}:{1}`".format(row["first_source_file"], row["first_line_number"])
            lines.append(
                "| `{0}` | `{1}` | {2} | {3} | {4} | {5} |".format(
                    row["action_key"],
                    row["cluster"],
                    row["count"],
                    row["errors"],
                    escape_md(row["recommended_action"]),
                    first_evidence,
                )
            )
        lines.append("")

    lines.append("## Top Findings")
    lines.append("")
    lines.append("| Severity | Category | Title | Recommendation |")
    lines.append("|---|---|---|---|")
    for finding in findings[: analyzer.args.top]:
        lines.append(
            "| `{0}` | `{1}` | {2} | {3} |".format(
                finding.severity,
                finding.category,
                escape_md(finding.title),
                escape_md(finding.recommendation),
            )
        )
    lines.append("")

    lines.append("## High ROI Backlog")
    lines.append("")
    high_roi_categories = {
        "maintenance_loop",
        "schema_fix",
        "needed_action",
        "escape_hatch_replacement",
        "expected_slow_domain_error",
        "child_query_bottleneck",
        "index_health",
        "large_result",
        "high_error_rate",
    }
    lines.append("| Rank | Category | Score | Recency | Evidence |")
    lines.append("|---:|---|---:|---|---|")
    rank = 0
    for finding in findings:
        if finding.category not in high_roi_categories:
            continue
        rank += 1
        sample = compact_message(stable_json(finding.sample), 160)
        lines.append(
            "| {0} | `{1}` | {2:.2f} | {3} | {4} |".format(
                rank, finding.category, finding.score, recency_label(finding), escape_md(sample)
            )
        )
        if rank >= analyzer.args.top:
            break
    lines.append("")

    recency_views = analyzer.recency_views(findings)
    for title, key in (
        ("Recency - Still Open", "still_open"),
        ("Recency - Regressions", "regressions"),
        ("Recency - Newly Quiet", "newly_quiet"),
    ):
        rows = recency_views.get(key, [])
        if not rows:
            continue
        lines.append("## {0}".format(title))
        lines.append("")
        lines.append("| Action | Category | Status | Recent err | Recent calls | Hist err | Last call | Recency score |")
        lines.append("|---|---|---|---:|---:|---:|---|---:|")
        for row in rows[: analyzer.args.top]:
            lines.append(
                "| `{0}` | `{1}` | `{2}` | {3} | {4} | {5} | `{6}` | {7:.2f} |".format(
                    row["action_key"],
                    row["category"],
                    row["status"],
                    row.get("recent_errors") or 0,
                    row.get("recent_calls") or 0,
                    row.get("historical_errors") or 0,
                    row.get("last_call_date") or "",
                    float(row.get("recency_score") or 0.0),
                )
            )
        lines.append("")

    lines.append("## Action Stats")
    lines.append("")
    lines.append("| Action | Count | Errors | Error rate | Noise | p95 ms | Total sec |")
    lines.append("|---|---:|---:|---:|---|---:|---:|")
    for row in analyzer.action_stats_rows()[: analyzer.args.top]:
        lines.append(
            "| `{0}` | {1} | {2} | {3:.3f} | `{4}` | {5:.2f} | {6:.2f} |".format(
                row["action_key"],
                row["count"],
                row["errors"],
                row["error_rate"],
                row["noise_class"],
                row["p95_ms"],
                row["total_ms"] / 1000.0,
            )
        )
    lines.append("")

    if analyzer.parse_warnings:
        lines.append("## Parse Warnings")
        lines.append("")
        lines.append("| File | Line | Kind | Message |")
        lines.append("|---|---:|---|---|")
        for warning in analyzer.parse_warnings[: analyzer.args.top]:
            lines.append(
                "| `{0}` | {1} | `{2}` | {3} |".format(
                    warning["source_file"],
                    warning["line_number"],
                    warning["kind"],
                    escape_md(warning["message"]),
                )
            )
        lines.append("")

    return "\n".join(lines)


def escape_md(text: str) -> str:
    return str(text).replace("|", "\\|").replace("\n", " ")


def recency_label(finding: Finding) -> str:
    """Compact recency cell for the High ROI Backlog table."""
    rec = finding.recency
    if not rec:
        return "-"
    status = rec.get("status", "")
    if rec.get("metric") == "calls":
        return "`{0}` (recent_calls={1})".format(status, rec.get("recent_calls") or 0)
    return "`{0}` (recent_err={1})".format(status, rec.get("recent_errors") or 0)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyze Monolith invocation JSONL logs.")
    parser.add_argument("--log-root", action="append", default=None, help="Log root or JSONL file. Repeatable.")
    parser.add_argument("--since", help="Include date folders on or after yyyyMMdd.")
    parser.add_argument("--until", help="Include date folders on or before yyyyMMdd.")
    parser.add_argument("--files", type=parse_surfaces, default=SURFACE_FILES, help="Comma list: proxy,action,query.")
    parser.add_argument("--out", type=Path, default=None, help="Output directory.")
    parser.add_argument("--format", type=parse_formats, default=DEFAULT_FORMATS, help="Comma list: markdown,json,csv.")
    parser.add_argument("--top", type=int, default=50, help="Maximum rows for ranked tables.")
    parser.add_argument("--max-lines", type=int, default=None, help="Stop after N records.")
    parser.add_argument("--category", action="append", default=[], help="Limit findings to a category. Repeatable.")
    parser.add_argument("--strict", action="store_true", help="Fail on malformed JSON or file read errors.")
    parser.add_argument("--emit-normalized-jsonl", action="store_true", help="Write normalized records.")
    parser.add_argument("--include-raw-snippets", action="store_true", help="Reserved for future bounded raw snippets.")
    parser.add_argument("--include-paths", action="store_true", help="Preserve full local paths in output.")
    parser.add_argument("--min-severity", default="info", choices=tuple(SEVERITY_RANK.keys()))
    parser.add_argument("--include-heartbeats", action="store_true", help="Include heartbeat records in ROI rankings.")
    parser.add_argument("--include-synthetic-tests", action="store_true", help="Include synthetic test rows in missing-action findings.")
    parser.add_argument(
        "--recent-days",
        type=int,
        default=DEFAULT_RECENT_DAYS,
        help="Recency window = last N present date folders (default {0}). Drives still_open and recency_score.".format(DEFAULT_RECENT_DAYS),
    )
    parser.add_argument(
        "--fix-boundary",
        default=None,
        help="yyyyMMdd; dates on/after the boundary count as 'recent'. Overrides --recent-days.",
    )
    parser.add_argument(
        "--rank-by-recency",
        action="store_true",
        help="Re-rank findings by recency-adjusted score (default off; legacy order is preserved).",
    )
    args = parser.parse_args(argv)
    args.log_root = [Path(item) for item in (args.log_root or ["Logs"])]
    if args.out is None:
        args.out = DEFAULT_OUTPUT_ROOT / timestamp_for_path()
    if args.top < 1:
        parser.error("--top must be >= 1")
    if args.recent_days < 1:
        parser.error("--recent-days must be >= 1")
    for attr in ("since", "until", "fix_boundary"):
        value = getattr(args, attr)
        if value and not re.fullmatch(r"\d{8}", value):
            parser.error("--{0} must use yyyyMMdd".format(attr.replace("_", "-")))
    if args.since and args.until and args.since > args.until:
        parser.error("--since must be <= --until")
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    repo_root = Path.cwd()
    try:
        ensure_output_dir(args.out, args.log_root)
    except ValueError as exc:
        print("FATAL: {0}".format(exc), file=sys.stderr)
        return 1
    files = discover_log_files(args.log_root, args.files, args.since, args.until)
    if not files:
        print("FATAL: no JSONL log files matched the requested inputs.", file=sys.stderr)
        return 1

    analyzer = Analyzer(repo_root, args)
    try:
        analyzer.analyze_files(files)
    except StrictParseError as exc:
        print("STRICT_PARSE_ERROR: {0}".format(exc), file=sys.stderr)
        return 2

    findings = analyzer.build_findings()
    write_reports(analyzer, files, findings)
    print(
        "Scanned {0} files, {1} records. Findings: {2}. Report: {3}".format(
            analyzer.files_scanned,
            analyzer.records_scanned,
            len(findings),
            args.out / "summary.md",
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
