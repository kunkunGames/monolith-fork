#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace monolith_project_search_query
{
	enum class projection_result
	{
		applicable,
		inapplicable,
		invalid
	};

	struct projection
	{
		projection_result result = projection_result::invalid;
		std::string query;
		std::string error;
	};

	namespace detail
	{
		// Recursive-descent precedence layers consume several frames per
		// parenthesized expression. Keep a generous bounded ceiling so normal
		// generated queries are accepted without exposing unbounded recursion.
		constexpr size_t max_parse_depth = 256;

		inline bool is_ascii_alnum(unsigned char value)
		{
			return (value >= static_cast<unsigned char>('a')
					&& value <= static_cast<unsigned char>('z'))
				|| (value >= static_cast<unsigned char>('A')
					&& value <= static_cast<unsigned char>('Z'))
				|| (value >= static_cast<unsigned char>('0')
					&& value <= static_cast<unsigned char>('9'));
		}

		struct decoded_code_point
		{
			uint32_t value = 0;
			size_t width = 0;
			bool valid = false;
		};

		inline bool is_utf8_continuation(unsigned char value)
		{
			return (value & 0xc0) == 0x80;
		}

		inline decoded_code_point decode_utf8(
			const std::string& value,
			size_t position)
		{
			if (position >= value.size())
			{
				return {};
			}

			const unsigned char first =
				static_cast<unsigned char>(value[position]);
			if (first <= 0x7f)
			{
				return {first, 1, true};
			}

			auto continuation = [&value](size_t index) -> int
			{
				if (index >= value.size())
				{
					return -1;
				}
				const unsigned char byte =
					static_cast<unsigned char>(value[index]);
				return is_utf8_continuation(byte)
					? static_cast<int>(byte & 0x3f)
					: -1;
			};

			if (first >= 0xc2 && first <= 0xdf)
			{
				const int second = continuation(position + 1);
				if (second >= 0)
				{
					return {
						static_cast<uint32_t>(
							(first & 0x1f) << 6 | second),
						2,
						true,
					};
				}
				return {};
			}

			if (first >= 0xe0 && first <= 0xef)
			{
				const int second = continuation(position + 1);
				const int third = continuation(position + 2);
				const bool valid_second =
					second >= 0
					&& (first != 0xe0 || second >= 0x20)
					&& (first != 0xed || second <= 0x1f);
				if (valid_second && third >= 0)
				{
					return {
						static_cast<uint32_t>(
							(first & 0x0f) << 12
							| second << 6
							| third),
						3,
						true,
					};
				}
				return {};
			}

			if (first >= 0xf0 && first <= 0xf4)
			{
				const int second = continuation(position + 1);
				const int third = continuation(position + 2);
				const int fourth = continuation(position + 3);
				const bool valid_second =
					second >= 0
					&& (first != 0xf0 || second >= 0x10)
					&& (first != 0xf4 || second <= 0x0f);
				if (valid_second && third >= 0 && fourth >= 0)
				{
					return {
						static_cast<uint32_t>(
							(first & 0x07) << 18
							| second << 12
							| third << 6
							| fourth),
						4,
						true,
					};
				}
			}
			return {};
		}

		inline bool is_bareword_code_point(uint32_t value)
		{
			// SQLite FTS5 barewords accept ASCII letters/digits, underscore,
			// U+001A, and every code point above U+007F.
			return value > 127
				|| value == 0x1a
				|| value == static_cast<uint32_t>('_')
				|| (value <= 0x7f
					&& is_ascii_alnum(static_cast<unsigned char>(value)));
		}

		inline bool is_bareword_at(
			const std::string& value,
			size_t position,
			size_t* out_width = nullptr)
		{
			const decoded_code_point decoded =
				decode_utf8(value, position);
			if (out_width)
			{
				*out_width = decoded.width;
			}
			return decoded.valid
				&& is_bareword_code_point(decoded.value);
		}

		inline bool is_bareword_before(
			const std::string& value,
			size_t position)
		{
			if (position == 0 || position > value.size())
			{
				return false;
			}

			size_t start = position - 1;
			size_t continuation_count = 0;
			while (start > 0
				   && is_utf8_continuation(
					   static_cast<unsigned char>(value[start]))
				   && continuation_count < 3)
			{
				--start;
				++continuation_count;
			}
			const decoded_code_point decoded = decode_utf8(value, start);
			return decoded.valid
				&& start + decoded.width == position
				&& is_bareword_code_point(decoded.value);
		}

		inline bool is_space(unsigned char value)
		{
			return value == static_cast<unsigned char>(' ')
				|| value == static_cast<unsigned char>('\t')
				|| value == static_cast<unsigned char>('\n')
				|| value == static_cast<unsigned char>('\r')
				|| value == static_cast<unsigned char>('\v')
				|| value == static_cast<unsigned char>('\f');
		}

		inline size_t utf8_character_index(
			const std::string& value,
			size_t byte_position)
		{
			size_t byte_index = 0;
			size_t character_index = 0;
			const size_t end = std::min(byte_position, value.size());
			while (byte_index < end)
			{
				const decoded_code_point decoded =
					decode_utf8(value, byte_index);
				const size_t width =
					decoded.valid && decoded.width > 0
						? decoded.width
						: 1;
				byte_index += std::min(width, end - byte_index);
				++character_index;
			}
			return character_index;
		}

		inline std::string trim(const std::string& value)
		{
			size_t begin = 0;
			size_t end = value.size();
			while (begin < end
				   && is_space(static_cast<unsigned char>(value[begin])))
			{
				++begin;
			}
			while (end > begin
				   && is_space(static_cast<unsigned char>(value[end - 1])))
			{
				--end;
			}
			return value.substr(begin, end - begin);
		}

		inline std::string normalize_identifier(std::string value)
		{
			for (char& character : value)
			{
				const unsigned char byte =
					static_cast<unsigned char>(character);
				if (byte >= static_cast<unsigned char>('A')
					&& byte <= static_cast<unsigned char>('Z'))
				{
					character = static_cast<char>(
						byte - static_cast<unsigned char>('A')
						+ static_cast<unsigned char>('a'));
				}
			}
			return value;
		}

		enum class node_type
		{
			leaf,
			group,
			filter,
			conjunction,
			disjunction,
			negation
		};

		struct node
		{
			node_type type = node_type::leaf;
			std::string text;
			std::vector<std::string> fields;
			bool exclude_fields = false;
			bool parenthesized_atom = false;
			std::unique_ptr<node> left;
			std::unique_ptr<node> right;

			static std::unique_ptr<node> make_leaf(std::string text)
			{
				auto result = std::make_unique<node>();
				result->type = node_type::leaf;
				result->text = std::move(text);
				return result;
			}

			static std::unique_ptr<node> make_unary(
				node_type type,
				std::unique_ptr<node> child,
				bool parenthesized_atom = false)
			{
				auto result = std::make_unique<node>();
				result->type = type;
				result->left = std::move(child);
				result->parenthesized_atom = parenthesized_atom;
				return result;
			}

			static std::unique_ptr<node> make_binary(
				node_type type,
				std::unique_ptr<node> left,
				std::unique_ptr<node> right)
			{
				auto result = std::make_unique<node>();
				result->type = type;
				result->left = std::move(left);
				result->right = std::move(right);
				return result;
			}
		};

		class parser
		{
		public:
			explicit parser(const std::string& query)
				: query_(query)
			{
			}

			std::unique_ptr<node> parse()
			{
				skip_whitespace();
				if (at_end())
				{
					fail("FTS5 query must not be empty");
					return nullptr;
				}

				std::unique_ptr<node> result = parse_disjunction(0);
				if (!result)
				{
					return nullptr;
				}
				skip_whitespace();
				if (!at_end())
				{
					fail_at("Unexpected token in FTS5 query", position_);
					return nullptr;
				}
				return result;
			}

			const std::string& error() const
			{
				return error_;
			}

		private:
			enum class filter_parse_result
			{
				none,
				parsed,
				invalid
			};

			struct parsed_filter
			{
				std::vector<std::string> fields;
				bool exclude = false;
			};

			const std::string& query_;
			size_t position_ = 0;
			std::string error_;

			bool at_end() const
			{
				return position_ >= query_.size();
			}

			void fail(const std::string& message)
			{
				if (error_.empty())
				{
					error_ = message;
				}
			}

			void fail_at(const std::string& message, size_t position)
			{
				if (error_.empty())
				{
					error_ = message + " at character "
						+ std::to_string(
							utf8_character_index(query_, position));
				}
			}

			bool skip_whitespace()
			{
				const size_t start = position_;
				while (!at_end()
					   && is_space(
						   static_cast<unsigned char>(query_[position_])))
				{
					++position_;
				}
				return position_ != start;
			}

			bool keyword_at(size_t position, const char* keyword) const
			{
				const std::string token(keyword);
				if (position + token.size() > query_.size()
					|| query_.compare(position, token.size(), token) != 0)
				{
					return false;
				}

				const bool left_boundary =
					position == 0
					|| !is_bareword_before(query_, position);
				const bool right_boundary =
					position + token.size() >= query_.size()
					|| !is_bareword_at(
						query_,
						position + token.size());
				return left_boundary && right_boundary;
			}

			bool consume_keyword(const char* keyword)
			{
				const size_t saved = position_;
				skip_whitespace();
				if (!keyword_at(position_, keyword))
				{
					position_ = saved;
					return false;
				}
				position_ += std::char_traits<char>::length(keyword);
				return true;
			}

			bool next_is_explicit_operator() const
			{
				return keyword_at(position_, "AND")
					|| keyword_at(position_, "OR")
					|| keyword_at(position_, "NOT");
			}

			std::unique_ptr<node> parse_disjunction(size_t depth)
			{
				if (depth > max_parse_depth)
				{
					fail("FTS5 query nesting exceeds the supported depth");
					return nullptr;
				}

				std::unique_ptr<node> left = parse_conjunction(depth + 1);
				if (!left)
				{
					return nullptr;
				}
				while (consume_keyword("OR"))
				{
					std::unique_ptr<node> right =
						parse_conjunction(depth + 1);
					if (!right)
					{
						fail("FTS5 OR requires an expression on both sides");
						return nullptr;
					}
					left = node::make_binary(
						node_type::disjunction,
						std::move(left),
						std::move(right));
				}
				return left;
			}

			std::unique_ptr<node> parse_conjunction(size_t depth)
			{
				std::unique_ptr<node> left = parse_negation(depth + 1);
				if (!left)
				{
					return nullptr;
				}
				while (consume_keyword("AND"))
				{
					std::unique_ptr<node> right =
						parse_negation(depth + 1);
					if (!right)
					{
						fail("FTS5 AND requires an expression on both sides");
						return nullptr;
					}
					left = node::make_binary(
						node_type::conjunction,
						std::move(left),
						std::move(right));
				}
				return left;
			}

			std::unique_ptr<node> parse_negation(size_t depth)
			{
				std::unique_ptr<node> left =
					parse_implicit_conjunction(depth + 1);
				if (!left)
				{
					return nullptr;
				}
				while (consume_keyword("NOT"))
				{
					std::unique_ptr<node> right =
						parse_implicit_conjunction(depth + 1);
					if (!right)
					{
						fail("FTS5 NOT requires an expression on both sides");
						return nullptr;
					}
					left = node::make_binary(
						node_type::negation,
						std::move(left),
						std::move(right));
				}
				return left;
			}

			std::unique_ptr<node> parse_implicit_conjunction(size_t depth)
			{
				std::unique_ptr<node> left = parse_atom(depth + 1);
				if (!left)
				{
					return nullptr;
				}

				for (;;)
				{
					const size_t separator = position_;
					if (!skip_whitespace()
						|| at_end()
						|| query_[position_] == ')'
						|| next_is_explicit_operator())
					{
						position_ = separator;
						break;
					}

					std::unique_ptr<node> right = parse_atom(depth + 1);
					if (!right)
					{
						return nullptr;
					}
					if (left->parenthesized_atom
						|| right->parenthesized_atom)
					{
						fail_at(
							"FTS5 does not allow implicit AND next to a parenthesized expression",
							separator);
						return nullptr;
					}
					left = node::make_binary(
						node_type::conjunction,
						std::move(left),
						std::move(right));
				}
				return left;
			}

			std::unique_ptr<node> parse_atom(size_t depth)
			{
				if (depth > max_parse_depth)
				{
					fail("FTS5 query nesting exceeds the supported depth");
					return nullptr;
				}

				skip_whitespace();
				if (at_end() || query_[position_] == ')')
				{
					return nullptr;
				}
				if (next_is_explicit_operator())
				{
					fail_at("Unexpected FTS5 boolean operator", position_);
					return nullptr;
				}

				parsed_filter filter;
				const filter_parse_result filter_result =
					try_parse_filter(filter);
				if (filter_result == filter_parse_result::invalid)
				{
					return nullptr;
				}
				const bool has_filter =
					filter_result == filter_parse_result::parsed;
				if (has_filter)
				{
					skip_whitespace();
				}

				const size_t expression_start = position_;
				bool has_initial_anchor = false;
				if (!at_end() && query_[position_] == '^')
				{
					has_initial_anchor = true;
					++position_;
					skip_whitespace();
				}

				std::unique_ptr<node> result;
				if (!at_end() && query_[position_] == '(')
				{
					if (has_initial_anchor)
					{
						fail_at(
							"FTS5 initial-token anchor may only prefix a phrase",
							expression_start);
						return nullptr;
					}
					++position_;
					std::unique_ptr<node> child =
						parse_disjunction(depth + 1);
					if (!child)
					{
						return nullptr;
					}
					skip_whitespace();
					if (at_end() || query_[position_] != ')')
					{
						fail_at(
							"Unterminated parenthesized FTS5 expression",
							expression_start);
						return nullptr;
					}
					++position_;
					result = node::make_unary(
						node_type::group,
						std::move(child),
						true);
				}
				else
				{
					const size_t near_saved = position_;
					std::string near_text;
					if (try_parse_near_group(near_text))
					{
						if (has_initial_anchor)
						{
							fail_at(
								"FTS5 initial-token anchor is not valid on a NEAR group",
								expression_start);
							return nullptr;
						}
						result = node::make_leaf(std::move(near_text));
					}
					else
					{
						position_ = near_saved;
						std::string ignored;
						if (!parse_string_token(ignored))
						{
							fail_at("Expected an FTS5 phrase", position_);
							return nullptr;
						}
						consume_optional_star();

						for (;;)
						{
							const size_t plus_saved = position_;
							skip_whitespace();
							if (at_end() || query_[position_] != '+')
							{
								position_ = plus_saved;
								break;
							}
							++position_;
							skip_whitespace();
							if (!parse_string_token(ignored))
							{
								fail_at(
									"FTS5 phrase '+' requires a following string",
									position_);
								return nullptr;
							}
							consume_optional_star();
						}

						result = node::make_leaf(trim(query_.substr(
							expression_start,
							position_ - expression_start)));
					}
				}

				if (has_filter)
				{
					const bool parenthesized_operand =
						result && result->parenthesized_atom;
					auto filtered = node::make_unary(
						node_type::filter,
						std::move(result),
						parenthesized_operand);
					filtered->fields = std::move(filter.fields);
					filtered->exclude_fields = filter.exclude;
					result = std::move(filtered);
				}
				return result;
			}

			filter_parse_result try_parse_filter(parsed_filter& out_filter)
			{
				const size_t saved = position_;
				out_filter = {};

				if (!at_end() && query_[position_] == '-')
				{
					out_filter.exclude = true;
					++position_;
					skip_whitespace();
				}

				if (at_end())
				{
					position_ = saved;
					return filter_parse_result::none;
				}

				if (query_[position_] == '{')
				{
					++position_;
					for (;;)
					{
						skip_whitespace();
						if (!at_end() && query_[position_] == '}')
						{
							if (out_filter.fields.empty())
							{
								fail_at(
									"FTS5 column set must contain at least one name",
									saved);
								return filter_parse_result::invalid;
							}
							++position_;
							break;
						}

						std::string field;
						if (!parse_string_token(field))
						{
							position_ = saved;
							return filter_parse_result::none;
						}
						out_filter.fields.push_back(
							normalize_identifier(std::move(field)));

						const size_t field_end = position_;
						if (!skip_whitespace()
							&& (at_end() || query_[position_] != '}'))
						{
							position_ = saved;
							return filter_parse_result::none;
						}
						if (position_ == field_end
							&& !at_end()
							&& query_[position_] != '}')
						{
							position_ = saved;
							return filter_parse_result::none;
						}
					}
				}
				else
				{
					std::string field;
					if (!parse_string_token(field))
					{
						position_ = saved;
						return filter_parse_result::none;
					}
					out_filter.fields.push_back(
						normalize_identifier(std::move(field)));
				}

				skip_whitespace();
				if (at_end() || query_[position_] != ':')
				{
					position_ = saved;
					return filter_parse_result::none;
				}
				++position_;
				return filter_parse_result::parsed;
			}

			bool parse_string_token(std::string& out_value)
			{
				out_value.clear();
				if (at_end())
				{
					return false;
				}

				if (query_[position_] == '"')
				{
					++position_;
					while (!at_end())
					{
						const char character = query_[position_++];
						if (character != '"')
						{
							out_value.push_back(character);
							continue;
						}
						if (!at_end() && query_[position_] == '"')
						{
							out_value.push_back('"');
							++position_;
							continue;
						}
						return true;
					}
					fail("Unterminated quoted FTS5 string");
					return false;
				}

				const size_t start = position_;
				while (!at_end())
				{
					size_t width = 0;
					if (!is_bareword_at(query_, position_, &width))
					{
						break;
					}
					position_ += width;
				}
				if (position_ == start)
				{
					return false;
				}
				out_value = query_.substr(start, position_ - start);
				return true;
			}

			void consume_optional_star()
			{
				const size_t saved = position_;
				skip_whitespace();
				if (!at_end() && query_[position_] == '*')
				{
					++position_;
				}
				else
				{
					position_ = saved;
				}
			}

			bool try_parse_near_group(std::string& out_text)
			{
				out_text.clear();
				const size_t start = position_;
				if (!keyword_at(position_, "NEAR"))
				{
					return false;
				}
				position_ += 4;
				skip_whitespace();
				if (at_end() || query_[position_] != '(')
				{
					position_ = start;
					return false;
				}

				int depth = 0;
				bool in_quote = false;
				for (; position_ < query_.size(); ++position_)
				{
					const char character = query_[position_];
					if (character == '"')
					{
						if (in_quote
							&& position_ + 1 < query_.size()
							&& query_[position_ + 1] == '"')
						{
							++position_;
							continue;
						}
						in_quote = !in_quote;
						continue;
					}
					if (in_quote)
					{
						continue;
					}
					if (character == '(')
					{
						++depth;
					}
					else if (character == ')')
					{
						--depth;
						if (depth == 0)
						{
							++position_;
							out_text = trim(query_.substr(
								start,
								position_ - start));
							return true;
						}
					}
				}
				fail_at("Unterminated FTS5 NEAR group", start);
				return false;
			}
		};

		struct projected_node
		{
			projection_result result = projection_result::invalid;
			std::string query;
			std::string error;
			int precedence = 4;
		};

		constexpr int precedence_disjunction = 1;
		constexpr int precedence_conjunction = 2;
		constexpr int precedence_negation = 3;
		constexpr int precedence_atom = 4;

		inline std::string render_child(
			const projected_node& child,
			int parent_precedence,
			bool parenthesize_equal = false)
		{
			const bool needs_parentheses =
				child.precedence < parent_precedence
				|| (parenthesize_equal
					&& child.precedence == parent_precedence);
			return needs_parentheses
				? "(" + child.query + ")"
				: child.query;
		}

		inline bool same_fields(
			const std::set<std::string>& left,
			const std::set<std::string>& right)
		{
			return left == right;
		}

		inline bool validate_fields(
			const node& value,
			const std::set<std::string>& enabled_fields,
			std::string& out_error,
			size_t depth)
		{
			if (depth > max_parse_depth)
			{
				out_error =
					"FTS5 query validation exceeds the supported depth";
				return false;
			}

			if (value.type == node_type::filter)
			{
				for (const std::string& field : value.fields)
				{
					if (enabled_fields.count(field) == 0)
					{
						out_error =
							"Unknown FTS5 column qualifier: " + field;
						return false;
					}
				}
			}

			if (value.left
				&& !validate_fields(
					*value.left,
					enabled_fields,
					out_error,
					depth + 1))
			{
				return false;
			}
			if (value.right
				&& !validate_fields(
					*value.right,
					enabled_fields,
					out_error,
					depth + 1))
			{
				return false;
			}
			return true;
		}

		inline std::string make_positive_filter(
			const std::set<std::string>& fields,
			const std::vector<std::string>& field_order,
			const std::string& child)
		{
			std::vector<std::string> ordered;
			for (const std::string& field : field_order)
			{
				if (fields.count(field) != 0)
				{
					ordered.push_back(field);
				}
			}

			std::string spec;
			if (ordered.size() == 1)
			{
				spec = ordered.front();
			}
			else
			{
				spec = "{";
				for (size_t index = 0; index < ordered.size(); ++index)
				{
					if (index > 0)
					{
						spec += " ";
					}
					spec += ordered[index];
				}
				spec += "}";
			}
			return spec + " : (" + child + ")";
		}

		inline projected_node project_node(
			const node& value,
			const std::set<std::string>& allowed_fields,
			const std::set<std::string>& enabled_fields,
			const std::vector<std::string>& current_field_order,
			size_t depth)
		{
			if (depth > max_parse_depth)
			{
				return {
					projection_result::invalid,
					{},
					"FTS5 query projection exceeds the supported depth",
				};
			}

			switch (value.type)
			{
			case node_type::leaf:
				if (allowed_fields.empty())
				{
					return {projection_result::inapplicable, {}, {}};
				}
				return {projection_result::applicable, value.text, {}};

			case node_type::group:
			{
				projected_node child = project_node(
					*value.left,
					allowed_fields,
					enabled_fields,
					current_field_order,
					depth + 1);
				if (child.result == projection_result::applicable)
				{
					child.query = "(" + child.query + ")";
					child.precedence = precedence_atom;
				}
				return child;
			}

			case node_type::filter:
			{
				std::set<std::string> requested;
				for (const std::string& raw_field : value.fields)
				{
					const std::string field =
						normalize_identifier(raw_field);
					if (enabled_fields.count(field) == 0)
					{
						return {
							projection_result::invalid,
							{},
							"Unknown FTS5 column qualifier: " + raw_field,
						};
					}
					requested.insert(field);
				}

				std::set<std::string> next_fields;
				if (value.exclude_fields)
				{
					next_fields = allowed_fields;
					for (const std::string& field : requested)
					{
						next_fields.erase(field);
					}
				}
				else
				{
					for (const std::string& field : allowed_fields)
					{
						if (requested.count(field) != 0)
						{
							next_fields.insert(field);
						}
					}
				}

				if (next_fields.empty())
				{
					return {projection_result::inapplicable, {}, {}};
				}

				projected_node child = project_node(
					*value.left,
					next_fields,
					enabled_fields,
					current_field_order,
					depth + 1);
				if (child.result != projection_result::applicable)
				{
					return child;
				}
				if (!same_fields(next_fields, allowed_fields))
				{
					child.query = make_positive_filter(
						next_fields,
						current_field_order,
						child.query);
					child.precedence = precedence_atom;
				}
				return child;
			}

			case node_type::conjunction:
			{
				projected_node left = project_node(
					*value.left,
					allowed_fields,
					enabled_fields,
					current_field_order,
					depth + 1);
				if (left.result != projection_result::applicable)
				{
					return left;
				}
				projected_node right = project_node(
					*value.right,
					allowed_fields,
					enabled_fields,
					current_field_order,
					depth + 1);
				if (right.result != projection_result::applicable)
				{
					return right;
				}
				return {
					projection_result::applicable,
					render_child(left, precedence_conjunction)
						+ " AND "
						+ render_child(right, precedence_conjunction),
					{},
					precedence_conjunction,
				};
			}

			case node_type::disjunction:
			{
				projected_node left = project_node(
					*value.left,
					allowed_fields,
					enabled_fields,
					current_field_order,
					depth + 1);
				if (left.result == projection_result::invalid)
				{
					return left;
				}
				projected_node right = project_node(
					*value.right,
					allowed_fields,
					enabled_fields,
					current_field_order,
					depth + 1);
				if (right.result == projection_result::invalid)
				{
					return right;
				}
				if (left.result == projection_result::inapplicable)
				{
					return right;
				}
				if (right.result == projection_result::inapplicable)
				{
					return left;
				}
				return {
					projection_result::applicable,
					render_child(left, precedence_disjunction)
						+ " OR "
						+ render_child(right, precedence_disjunction),
					{},
					precedence_disjunction,
				};
			}

			case node_type::negation:
			{
				projected_node left = project_node(
					*value.left,
					allowed_fields,
					enabled_fields,
					current_field_order,
					depth + 1);
				if (left.result != projection_result::applicable)
				{
					return left;
				}
				projected_node right = project_node(
					*value.right,
					allowed_fields,
					enabled_fields,
					current_field_order,
					depth + 1);
				if (right.result == projection_result::invalid)
				{
					return right;
				}
				if (right.result == projection_result::inapplicable)
				{
					return left;
				}
				return {
					projection_result::applicable,
					render_child(left, precedence_negation)
						+ " NOT "
						+ render_child(
							right,
							precedence_negation,
							true),
					{},
					precedence_negation,
				};
			}
			}

			return {
				projection_result::invalid,
				{},
				"Unknown FTS5 projection node",
			};
		}
	}

	inline projection project(
		const std::string& query,
		const std::vector<std::string>& current_fields,
		const std::set<std::string>& enabled_fields)
	{
		detail::parser parser(query);
		std::unique_ptr<detail::node> root = parser.parse();
		if (!root)
		{
			return {
				projection_result::invalid,
				{},
				parser.error().empty()
					? "Malformed FTS5 query"
					: parser.error(),
			};
		}

		std::vector<std::string> normalized_order;
		std::set<std::string> normalized_current;
		for (const std::string& raw_field : current_fields)
		{
			const std::string field =
				detail::normalize_identifier(raw_field);
			normalized_order.push_back(field);
			normalized_current.insert(field);
		}

		std::set<std::string> normalized_enabled;
		for (const std::string& raw_field : enabled_fields)
		{
			normalized_enabled.insert(
				detail::normalize_identifier(raw_field));
		}

		std::string validation_error;
		if (!detail::validate_fields(
				*root,
				normalized_enabled,
				validation_error,
				0))
		{
			return {
				projection_result::invalid,
				{},
				std::move(validation_error),
			};
		}

		detail::projected_node result = detail::project_node(
			*root,
			normalized_current,
			normalized_enabled,
			normalized_order,
			0);
		return {
			result.result,
			std::move(result.query),
			std::move(result.error),
		};
	}
}
