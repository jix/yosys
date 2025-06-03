/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2025  Jannis Harder <jix@yosyshq.com> <me@jix.one>
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"

#include "frontends/verific/verific.h"

USING_YOSYS_NAMESPACE

PRIVATE_NAMESPACE_BEGIN

template <class T, class... A>
std::string format(T const &item, A &&...args)
{
	std::string target;
	item.format_into(target, std::forward<decltype(args)>(args)...);
	return target;
}

template <class T>
struct StableVec {
	std::vector<std::vector<T>> chunks;
	std::vector<T *> flat;

	T const &operator[](size_t index) const
	{
		log_assert(index < flat.size());
		return *flat[index];
	}

	T &operator[](size_t index)
	{
		log_assert(index < flat.size());
		return *flat[index];
	}

	size_t size() const { return flat.size(); }

	template <class... A>
	void emplace_back(A &&...args)
	{
		if (chunks.empty()) {
			chunks.emplace_back();
			chunks.back().reserve(32);
		} else if (chunks.back().capacity() == chunks.back().size()) {
			size_t cap = chunks.back().capacity();
			chunks.emplace_back();
			chunks.back().reserve(cap * 2);
		}
		chunks.back().emplace_back(std::forward<decltype(args)>(args)...);
		flat.push_back(&chunks.back().back());
	}
};

struct EmptyVariant {
	EmptyVariant() {}

	void format_into(std::string &target) const { target += "-empty-"; }

	template <class T>
	void format_into(std::string &target, T *context = nullptr) const
	{
		(void)context;
		format_into(target);
	}
	std::string dot_node_color() const { return "white"; }
};

struct RepetitionRange {
	int min;
	int max;

	RepetitionRange() : min(0), max(0) {}
	RepetitionRange(int delay) : min(delay), max(delay) { log_assert(delay >= 0); }

	RepetitionRange(int delay, std::nullopt_t) : min(delay), max(-1) { log_assert(delay >= 0); }
	RepetitionRange(int min, int max) : min(min), max(max)
	{
		log_assert(min >= 0);
		log_assert(max >= min);
	}

	static RepetitionRange at_least(int delay) { return {delay, std::nullopt}; }
	static RepetitionRange star() { return at_least(0); }
	static RepetitionRange plus() { return at_least(1); }
	static RepetitionRange parse(const char *min_str, const char *max_str)
	{
		if (!strcmp(max_str, "$")) {
			return at_least(atoi(min_str));
		} else {
			return RepetitionRange(atoi(min_str), atoi(max_str));
		}
	}
	static RepetitionRange parse(Verific::Instance *inst) { return parse(inst->GetAttValue("sva:low"), inst->GetAttValue("sva:high")); }

	RepetitionRange operator-(int offset) const
	{
		log_assert(offset >= 0);
		int new_min = min <= offset ? 0 : min - offset;
		if (max == -1) {
			return at_least(new_min);
		} else {
			log_assert(max >= offset);
			return {new_min, max - offset};
		}
	}

	int known_finite_constant() const
	{
		log_assert(min == max);
		return min;
	}

	bool contains(int i) const { return i >= min && (max < 0 || i <= max); }

	void format_into(std::string &target, const char *prefix = "") const
	{
		if (min == max) {
			if (prefix[0]) {
				target += stringf("[%s%d]", prefix, min);
			} else {
				target += std::to_string(min);
			}
		} else if (max == -1) {
			if (min == 0 && !prefix[0])
				target += stringf("[%s*]", prefix);
			else if (min == 0 && prefix[0] == '*')
				target += stringf("[%s]", prefix);
			else if (min == 1 && !prefix[0])
				target += stringf("[%s+]", prefix);
			else if (min == 1 && prefix[0] == '*')
				target += stringf("[+%s]", prefix + 1);
			else
				target += stringf("[%s%d:$]", prefix, min);
		} else {
			target += stringf("[%s%d:%d]", prefix, min, max);
		}
	}
};

struct Ast;

struct AstSeqId {
	int index = -1;

	AstSeqId() {}
	explicit AstSeqId(int index) : index(index) {}

	bool operator==(const AstSeqId &other) const { return index == other.index; }
	bool operator!=(const AstSeqId &other) const { return index != other.index; }
	[[nodiscard]] Hasher hash_into(Hasher h) const
	{
		h.eat(index);
		return h;
	}

	void format_into(std::string &target, Ast *ast = nullptr) const;
};

struct AstPropId {
	int index = -1;

	AstPropId() {}
	explicit AstPropId(int index) : index(index) {}

	bool operator==(const AstPropId &other) const { return index == other.index; }
	bool operator!=(const AstPropId &other) const { return index != other.index; }
	[[nodiscard]] Hasher hash_into(Hasher h) const
	{
		h.eat(index);
		return h;
	}

	void format_into(std::string &target, Ast *ast = nullptr) const;
};

struct AstUnhandled {
	std::string message;
	std::array<std::variant<AstSeqId, AstPropId>, 2> asts = {};

	void format_into(std::string &target, Ast *ast = nullptr) const;
};

struct AstBool {
	SigBit atom;
	bool value = true;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		(void)ast;
		if (!value)
			target += '!';
		target += log_signal(atom);
	}

	bool is_constant() const { return !atom.is_wire() && (atom.data == State::S0 || atom.data == State::S1); }
	bool constant_value() const
	{
		log_assert(is_constant());
		return value ? atom == State::S1 : atom == State::S0;
	}
};

struct AstBoolSeq : public AstBool {
};

struct AstConcatSeq {
	std::array<AstSeqId, 2> seqs;
	RepetitionRange delay;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		seqs[0].format_into(target, ast);
		target += " ##";
		delay.format_into(target);
		target += ' ';
		seqs[1].format_into(target, ast);
	}
};

struct AstDelaySeq {
	RepetitionRange delay;
	AstSeqId seq;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		target += "##";
		delay.format_into(target);
		target += ' ';
		seq.format_into(target, ast);
	}
};

struct AstRepeatSeq {
	enum class Type {
		CONSECUTIVE,
		NONCONSECUTIVE,
		GOTO,
		OVERLAPPING, // used internally during lowering
	} type;
	static constexpr Type CONSECUTIVE = Type::CONSECUTIVE;
	static constexpr Type NONCONSECUTIVE = Type::NONCONSECUTIVE;
	static constexpr Type GOTO = Type::GOTO;
	static constexpr Type OVERLAPPING = Type::OVERLAPPING;

	AstSeqId seq;
	RepetitionRange repeats;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		seq.format_into(target, ast);
		target += ' ';
		switch (type) {
		case GOTO:
			repeats.format_into(target, "->");
			break;
		case NONCONSECUTIVE:
			repeats.format_into(target, "=");
			break;
		case CONSECUTIVE:
			repeats.format_into(target, "*");
			break;
		case OVERLAPPING:
			repeats.format_into(target, "#-#");
			break;
		}
	}
};

struct AstBinaryOpSeq {
	enum class Type {
		OR,
		AND,
		THROUGHOUT,
		INTERSECT,
		WITHIN,
	} type;
	static constexpr Type OR = Type::OR;
	static constexpr Type AND = Type::AND;
	static constexpr Type THROUGHOUT = Type::THROUGHOUT;
	static constexpr Type INTERSECT = Type::INTERSECT;
	static constexpr Type WITHIN = Type::WITHIN;

	std::array<AstSeqId, 2> seqs;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		seqs[0].format_into(target, ast);
		switch (type) {
		case OR: {
			target += " or ";
		} break;
		case AND: {
			target += " and ";
		} break;
		case THROUGHOUT: {
			target += " throughout ";
		} break;
		case INTERSECT: {
			target += " intersect ";
		} break;
		case WITHIN: {
			target += " within ";
		} break;
		}
		seqs[1].format_into(target, ast);
	}
};

struct AstFirstMatchSeq {
	AstSeqId seq;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		target += "first_match ";
		seq.format_into(target, ast);
	}
};

typedef std::variant<EmptyVariant, AstBoolSeq, AstConcatSeq, AstDelaySeq, AstRepeatSeq, AstBinaryOpSeq, AstFirstMatchSeq, AstUnhandled>
  AstSeqNodeVariant;

struct AstSeqNode : public AstSeqNodeVariant {
	using AstSeqNodeVariant::variant;

	AstSeqNode() : AstSeqNodeVariant::variant(EmptyVariant()) {}

	bool empty() const { return std::holds_alternative<EmptyVariant>(*this); }

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		std::visit([&](auto &&arg) { arg.format_into(target, ast); }, *this);
	}
};

struct AstSeqProp {
	AstSeqId seq;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		target += "/* seq */ ";
		seq.format_into(target, ast);
	}
};

struct AstClockProp {
	std::shared_ptr<VerificClocking> clocking;
	AstPropId prop;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		target += "@(/* TODO */) ";
		prop.format_into(target, ast);
	}
};

struct AstBoolProp : public AstBool {
};

struct AstImplProp {
	enum class Type { IMPL, FOLLOWED_BY } type;
	static constexpr Type IMPL = Type::IMPL;
	static constexpr Type FOLLOWED_BY = Type::FOLLOWED_BY;

	enum class Overlap { OVERLAPPING, NONOVERLAPPING } overlap;
	static constexpr Overlap OVERLAPPING = Overlap::OVERLAPPING;
	static constexpr Overlap NONOVERLAPPING = Overlap::NONOVERLAPPING;

	AstSeqId seq;
	AstPropId prop;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		char mid[8] = " ??? ";
		mid[1] = type == IMPL ? '|' : '#';
		mid[2] = overlap == OVERLAPPING ? '-' : '=';
		mid[3] = type == IMPL ? '>' : '#';

		seq.format_into(target, ast);
		target += mid;
		prop.format_into(target, ast);
	}
};

AstImplProp::Type operator!(AstImplProp::Type type)
{
	return type == AstImplProp::Type::IMPL ? AstImplProp::Type::FOLLOWED_BY : AstImplProp::Type::IMPL;
};

struct AstNotProp {
	AstPropId prop;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		target += "not ";
		prop.format_into(target, ast);
	}
};

struct AstLogicProp {
	enum class Type { AND, OR, IMPLIES, IFF } type;
	static constexpr Type AND = Type::AND;
	static constexpr Type OR = Type::OR;
	static constexpr Type IMPLIES = Type::IMPLIES;
	static constexpr Type IFF = Type::IFF;

	std::array<AstPropId, 2> prop;
	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		prop[0].format_into(target, ast);
		switch (type) {
		case AND: {
			target += " and ";
		} break;
		case OR: {
			target += " or ";
		} break;
		case IMPLIES: {
			target += " implies ";
		} break;
		case IFF: {
			target += " iff ";
		} break;
		}
		prop[1].format_into(target, ast);
	}
};

struct AstNexttimeProp {
	int ticks;
	AstPropId prop;

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		target += "nexttime";
		if (ticks != 1)
			target += stringf("[%d]", ticks);
		target += ' ';
		prop.format_into(target, ast);
	}
};

typedef std::variant<EmptyVariant, AstSeqProp, AstBoolProp, AstClockProp, AstImplProp, AstNotProp, AstLogicProp, AstNexttimeProp, AstUnhandled>
  AstPropNodeVariant;

struct AstPropNode : public AstPropNodeVariant {
	using AstPropNodeVariant::variant;

	AstPropNode() : AstPropNodeVariant::variant(EmptyVariant()) {}

	bool empty() const { return std::holds_alternative<EmptyVariant>(*this); }

	void format_into(std::string &target, Ast *ast = nullptr) const
	{
		std::visit([&](auto &&arg) { arg.format_into(target, ast); }, *this);
	}
};

static std::string dot_escape_html(
  std::string content, std::function<void(std::string &, char)> escape_code_handler = [](std::string &target, char c) {
	  if (c == '\n')
		  target += "</td></tr><tr><td>";
  })
{
	std::string escaped;
	for (char c : content) {
		if (c <= '\n') {
			escape_code_handler(escaped, c);
			continue;
		}
		switch (c) {
		case '<': {
			escaped += "&lt;";
		} break;
		case '>': {
			escaped += "&gt;";
		} break;
		case '&': {
			escaped += "&amp;";
		} break;
		case '\\': {
			escaped += "\\\\";
		} break;
		default:
			escaped += c;
		}
	}
	return escaped;
}

static std::string dot_table_box(std::string escaped_inner, bool border = true)
{
	return stringf("<table border=\"%d\" cellborder=\"0\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
		       "<td height=\"2\"></td></tr><tr><td> </td><td>"
		       "<table border=\"0\" cellborder=\"0\" cellspacing=\"0\" cellpadding=\"0\"><tr><td>"
		       "%s"
		       "</td></tr></table>"
		       "</td><td> </td></tr></table>",
		       (int)border, escaped_inner.c_str());
}

static std::string dot_escape_with_ports(std::string with_ports)
{
	int port_count = 0;

	static const char *open_table = "<table border=\"0\" cellborder=\"0\" cellspacing=\"0\" cellpadding=\"0\"><tr><td>";
	static const char *close_table = "</td></tr></table>";

	std::string escaped = dot_escape_html(with_ports, [&](std::string &target, char c) {
		if (c == 1) {
			target += stringf("</td><td port=\"c%d\">", port_count++);
		} else if (c == 2) {
			target += "</td><td>";
		} else if (c == '\n') {
			target += close_table;
			target += "</td></tr><tr><td>";
			target += open_table;
		}
	});

	return open_table + escaped + close_table;
}

struct Ast {
	StableVec<AstSeqNode> seqs;
	StableVec<AstPropNode> props;

	AstSeqNode const &operator[](AstSeqId id) const
	{
		log_assert(0 <= id.index && id.index < GetSize(seqs) && !seqs[id.index].empty());
		return seqs[id.index];
	}
	AstPropNode &operator[](AstPropId id)
	{
		log_assert(0 <= id.index && id.index < GetSize(props) && !props[id.index].empty());
		return props[id.index];
	}

	template <class... T>
	AstSeqId new_seq(T &&...args)
	{
		AstSeqId new_id(GetSize(seqs));
		seqs.emplace_back(std::forward<decltype(args)>(args)...);
		return new_id;
	}

	template <class... T>
	AstPropId new_prop(T &&...args)
	{
		AstPropId new_id(GetSize(props));
		props.emplace_back(std::forward<decltype(args)>(args)...);
		return new_id;
	}

	void format_into(std::string &target)
	{
		for (int i = 0; i < GetSize(seqs); ++i) {
			if (seqs[i].empty())
				continue;
			AstSeqId(i).format_into(target);
			target += " = ";
			seqs[i].format_into(target);
			target += '\n';
		}
		for (int i = 0; i < GetSize(props); ++i) {
			if (props[i].empty())
				continue;
			AstPropId(i).format_into(target);
			target += " = ";
			props[i].format_into(target);
			target += '\n';
		}
	}

	bool dot_mode = false;
	std::vector<std::variant<AstSeqId, AstPropId>> dot_links;

	void write_dot_into(std::string const &path, std::optional<std::variant<AstSeqId, AstPropId>> root = std::nullopt)
	{
		std::ofstream out(path);
		out << "digraph {\n";
		out << "  node [shape=plain, style=filled];\n";
		dot_mode = true;

		write_dot_nodes<AstSeqId, AstSeqNode>(out, seqs, "#ccccff");
		write_dot_nodes<AstPropId, AstPropNode>(out, props, "#ffffaa");

		if (root.has_value()) {
			out << "  root [label=<root>,style=solid];\n";
			out << stringf("  root -> %s;\n", std::visit([&](auto &&arg) { return format(arg); }, root.value()).c_str());
		}

		dot_mode = false;
		out << "}\n";
	}

	template <class Id, class Node>
	void write_dot_nodes(std::ostream &out, StableVec<Node> const &nodes, const char *color)
	{
		for (int i = 0; i < GetSize(nodes); ++i) {
			if (nodes[i].empty())
				continue;
			dot_links.clear();

			std::string node_name = format(Id(i));
			std::string label = dot_escape_html(node_name + "\n") + dot_escape_with_ports(format(nodes[i], this));

			out << stringf("  %s [label=<%s>,fillcolor=\"%s\"];\n", node_name.c_str(), dot_table_box(label).c_str(), color);

			int link_nr = 0;
			for (auto dot_link : dot_links) {
				std::string link_name = std::visit([&](auto &&arg) { return format(arg); }, dot_link);
				out << stringf("  %s:c%d -> %s;\n", node_name.c_str(), link_nr++, link_name.c_str());
			}
		}
	}

	dict<AstSeqId, bool> admits_empty_cache;

	bool admits_empty(AstSeqId seq_id)
	{
		auto it = admits_empty_cache.find(seq_id);
		if (it != admits_empty_cache.end())
			return it->second;

		bool result = std::visit([&](auto &&seq) { return admits_empty(seq, seq_id); }, (*this)[seq_id]);
		admits_empty_cache[seq_id] = result;
		return result;
	}

	bool admits_empty(AstBoolSeq const &seq, AstSeqId seq_id) { return false; }

	bool admits_empty(AstConcatSeq const &seq, AstSeqId seq_id)
	{
		return seq.delay.contains(1) && admits_empty(seq.seqs[0]) && admits_empty(seq.seqs[1]);
	}

	bool admits_empty(AstRepeatSeq const &seq, AstSeqId seq_id)
	{
		// I haven't double checked whether this is correct for non-consecutive
		// repetitions of non-boolean sequences, which are not valid SVA input
		// syntax.
		return seq.type != AstRepeatSeq::OVERLAPPING && (seq.repeats.min == 0 || admits_empty(seq.seq));
	}

	bool admits_empty(AstBinaryOpSeq const &seq, AstSeqId seq_id)
	{
		switch (seq.type) {
		case AstBinaryOpSeq::OR:
			return admits_empty(seq.seqs[0]) || admits_empty(seq.seqs[1]);
		case AstBinaryOpSeq::AND:
		case AstBinaryOpSeq::INTERSECT:
		case AstBinaryOpSeq::WITHIN:
			return admits_empty(seq.seqs[0]) && admits_empty(seq.seqs[1]);
		case AstBinaryOpSeq::THROUGHOUT:
			return admits_empty(seq.seqs[1]);
		}
	}

	bool admits_empty(AstFirstMatchSeq const &seq, AstSeqId seq_id) { return admits_empty(seq.seq); }

	template <class T>
	bool admits_empty(T const &seq, AstSeqId seq_id)
	{
		log_warning("missing admits_empty implementation for %s = %s\n", format(seq_id).c_str(), format(seq).c_str());
		return false;
	}

	dict<AstSeqId, AstSeqId> lower_seq_cache;
	dict<AstPropId, AstPropId> lower_prop_cache;

	AstSeqId lower(AstSeqId seq_id)
	{
		auto it = lower_seq_cache.find(seq_id);
		if (it != lower_seq_cache.end())
			return it->second;

		auto result = std::visit([&](auto &&seq) { return lower(seq, seq_id); }, (*this)[seq_id]);
		lower_seq_cache[seq_id] = result;
		return result;
	}

	AstSeqId lower(AstBinaryOpSeq const &seq, AstSeqId seq_id)
	{
		switch (seq.type) {
		case AstBinaryOpSeq::OR:
		case AstBinaryOpSeq::INTERSECT:
			return seq_id;
		case AstBinaryOpSeq::AND:
		case AstBinaryOpSeq::WITHIN:
			return seq_id; // TODO
		case AstBinaryOpSeq::THROUGHOUT:
			return new_seq(
			  AstBinaryOpSeq{AstBinaryOpSeq::INTERSECT,
					 {{new_seq(AstRepeatSeq{AstRepeatSeq::CONSECUTIVE, seq.seqs[0], RepetitionRange::star()}), seq.seqs[1]}}});
		}
	}

	AstSeqId lower(AstRepeatSeq const &seq, AstSeqId seq_id)
	{
		if (admits_empty(seq_id) || seq.type != AstRepeatSeq::CONSECUTIVE)
			return seq_id; // TODO
		return new_seq(
		  AstConcatSeq{{{seq.seq, new_seq(AstRepeatSeq{AstRepeatSeq::OVERLAPPING, new_seq(AstDelaySeq{1, seq.seq}), seq.repeats - 1})}}, 0});
	}

	template <class T>
	AstSeqId lower(T const &seq, AstSeqId seq_id)
	{
		(void)seq;
		return seq_id;
	}

	AstPropId lower(AstPropId prop)
	{
		auto it = lower_prop_cache.find(prop);
		if (it != lower_prop_cache.end())
			return it->second;

		auto result = std::visit([&](auto &&arg) { return lower(arg, prop); }, (*this)[prop]);
		lower_prop_cache[prop] = result;
		return result;
	}

	AstPropId lower(AstNotProp const &prop, AstPropId prop_id)
	{
		(void)prop_id;
		return negated(prop.prop);
	}

	AstPropId lower(AstSeqProp const &prop, AstPropId prop_id)
	{
		(void)prop_id;
		return new_prop(AstImplProp{AstImplProp::FOLLOWED_BY, AstImplProp::OVERLAPPING, prop.seq, new_prop(AstBoolProp{State::S1})});
	}

	AstPropId lower(AstLogicProp const &prop, AstPropId prop_id)
	{
		switch (prop.type) {
		case AstLogicProp::AND:
		case AstLogicProp::OR:
			return prop_id;
		case AstLogicProp::IMPLIES:
			return new_prop(AstLogicProp{AstLogicProp::OR, {{negated(prop.prop[0]), prop.prop[1]}}});
		case AstLogicProp::IFF:
			return new_prop(AstLogicProp{AstLogicProp::AND,
						     {{new_prop(AstLogicProp{AstLogicProp::IMPLIES, {{prop.prop[0], prop.prop[1]}}}),
						       new_prop(AstLogicProp{AstLogicProp::IMPLIES, {{prop.prop[1], prop.prop[0]}}})}}});
		}
	}

	template <class T>
	AstPropId lower(T const &prop, AstPropId prop_id)
	{
		(void)prop;
		return prop_id;
	}

	dict<AstPropId, AstPropId> negated_cache;

	AstPropId negated(AstPropId prop)
	{
		auto it = negated_cache.find(prop);
		if (it != negated_cache.end())
			return it->second;

		auto result = std::visit([&](auto &&arg) { return negated(arg, prop); }, (*this)[prop]);
		negated_cache[prop] = result;
		if (!std::holds_alternative<AstNotProp>((*this)[prop])) {
			negated_cache[result] = prop;
		}
		return result;
	}

	AstPropId negated(AstNotProp const &prop, AstPropId prop_id)
	{
		(void)prop_id;
		return prop.prop;
	}

	AstPropId negated(AstClockProp const &prop, AstPropId prop_id)
	{
		(void)prop_id;
		return new_prop(AstClockProp{prop.clocking, negated(prop.prop)});
	}

	AstPropId negated(AstImplProp const &prop, AstPropId prop_id)
	{
		(void)prop_id;
		return new_prop(AstImplProp{!prop.type, prop.overlap, prop.seq, negated(prop.prop)});
	}

	AstPropId negated(AstSeqProp const &prop, AstPropId prop_id)
	{
		(void)prop_id;
		return new_prop(AstImplProp{AstImplProp::IMPL, AstImplProp::OVERLAPPING, prop.seq, new_prop(AstBoolProp{SigBit(State::S0)})});
	}

	AstPropId negated(AstNexttimeProp const &prop, AstPropId prop_id)
	{
		(void)prop_id;
		return new_prop(AstNexttimeProp{prop.ticks, negated(prop.prop)});
	}

	template <class T>
	AstPropId negated(T const &prop, AstPropId prop_id)
	{
		std::string message = stringf("unhandled variant: negated: %s = %s", format(prop_id).c_str(), format(prop).c_str());
		log_warning("%s\n", message.c_str());
		return new_prop(AstUnhandled{message, {prop_id}});
	}
};

void AstSeqId::format_into(std::string &target, Ast *ast) const
{
	if (!ast) {
		target += stringf("seq_%d", index);
	} else if (ast->dot_mode) {
		target += stringf("\x01seq_%d\x02", index);
		ast->dot_links.push_back(*this);
	} else {
		target += stringf("( /* seq_%d */ ", index);
		(*ast)[*this].format_into(target);
		target += ')';
	}
}

void AstPropId::format_into(std::string &target, Ast *ast) const
{
	if (!ast) {
		target += stringf("prop_%d", index);
		if (ast)
			ast->dot_links.push_back(*this);
	} else if (ast->dot_mode) {
		target += stringf("\x01prop_%d\x02", index);
		ast->dot_links.push_back(*this);
	} else {
		target += stringf("( /* prop_%d */ ", index);
		(*ast)[*this].format_into(target, ast);
		target += ')';
	}
}

void AstUnhandled::format_into(std::string &target, Ast *ast) const
{
	if (ast && ast->dot_mode) {
		int pos = 0;
		while (true) {
			int found = message.find(": ", pos);
			if (found < 0) {
				target.append(message.begin() + pos, message.end());
				target.append("\n");
				break;
			}
			target.append(message.begin() + pos, message.begin() + found);
			target.append("\n");
			pos = found + 2;
		}

		for (auto node : asts) {
			std::visit(
			  [&](auto &&node) {
				  if (node.index < 0)
					  return;
				  target += ' ';
				  node.format_into(target, ast);
			  },
			  node);
		}
		if (target.back() == '\n')
			target.pop_back();
	} else {
		target += message;
	}
}

struct VerificToAstWorker {
	VerificImporter *importer = nullptr;
	Ast ast;
	Verific::Instance *root;
	AstPropId ast_root;

	void import()
	{
		using namespace Verific;
		log("  [new] importing SVA property at root cell %s (%s) at %s:%d.\n", root->Name(), root->View()->Owner()->Name(),
		    LineFile::GetFileName(root->Linefile()), LineFile::GetLineNo(root->Linefile()));

		ast_root = parse_property(root->GetInput());
	}

	dict<Verific::Net *, AstSeqId> parse_sequence_cache;

	AstSeqId parse_sequence(Verific::Net *net)
	{
		auto it = parse_sequence_cache.find(net);
		if (it != parse_sequence_cache.end())
			return it->second;

		AstSeqId result = parse_sequence_uncached(net);
		parse_sequence_cache[net] = result;
		return result;
	}

	AstSeqId parse_sequence_uncached(Verific::Net *net)
	{
		using namespace Verific;
		log_assert(net);

		prim_type type = PRIM_END;
		Instance *inst = nullptr;
		if (!net->IsMultipleDriven() && (inst = net->Driver()))
			type = inst->Type();
		switch (type) {
		case PRIM_GND:
			return ast.new_seq(AstBoolSeq{SigBit(State::S0)});
		case PRIM_PWR:
			return ast.new_seq(AstBoolSeq{SigBit(State::S1)});
		case PRIM_SVA_SEQ_CONCAT:
			return ast.new_seq(
			  AstConcatSeq{{{parse_sequence(inst->GetInput1()), parse_sequence(inst->GetInput2())}}, RepetitionRange::parse(inst)});
		case PRIM_SVA_CONSECUTIVE_REPEAT:
			return ast.new_seq(AstRepeatSeq{AstRepeatSeq::CONSECUTIVE, parse_sequence(inst->GetInput()), RepetitionRange::parse(inst)});
		case PRIM_SVA_NON_CONSECUTIVE_REPEAT:
			return ast.new_seq(
			  AstRepeatSeq{AstRepeatSeq::NONCONSECUTIVE, parse_sequence(inst->GetInput()), RepetitionRange::parse(inst)});
		case PRIM_SVA_GOTO_REPEAT:
			return ast.new_seq(AstRepeatSeq{AstRepeatSeq::GOTO, parse_sequence(inst->GetInput()), RepetitionRange::parse(inst)});
		case PRIM_SVA_SEQ_AND:
			return ast.new_seq(
			  AstBinaryOpSeq{AstBinaryOpSeq::AND, {parse_sequence(inst->GetInput1()), parse_sequence(inst->GetInput2())}});
		case PRIM_SVA_SEQ_OR:
			return ast.new_seq(
			  AstBinaryOpSeq{AstBinaryOpSeq::OR, {parse_sequence(inst->GetInput1()), parse_sequence(inst->GetInput2())}});
		case PRIM_SVA_THROUGHOUT:
			return ast.new_seq(
			  AstBinaryOpSeq{AstBinaryOpSeq::THROUGHOUT, {parse_sequence(inst->GetInput1()), parse_sequence(inst->GetInput2())}});
		case PRIM_SVA_INTERSECT:
			return ast.new_seq(
			  AstBinaryOpSeq{AstBinaryOpSeq::INTERSECT, {parse_sequence(inst->GetInput1()), parse_sequence(inst->GetInput2())}});
		case PRIM_SVA_WITHIN:
			return ast.new_seq(
			  AstBinaryOpSeq{AstBinaryOpSeq::WITHIN, {parse_sequence(inst->GetInput1()), parse_sequence(inst->GetInput2())}});
		case PRIM_SVA_FIRST_MATCH:
			return ast.new_seq(AstFirstMatchSeq{parse_sequence(inst->GetInput())});

		default: {
			// handled below
		}
		}

		// clang-format off
		switch (type) {
			case PRIM_SVA_IMMEDIATE_ASSERT: case PRIM_SVA_ASSERT: case PRIM_SVA_COVER:
			case PRIM_SVA_ASSUME: case PRIM_SVA_EXPECT: case PRIM_SVA_POSEDGE:
			case PRIM_SVA_NOT: case PRIM_SVA_FIRST_MATCH: case PRIM_SVA_ENDED:
			case PRIM_SVA_MATCHED: case PRIM_SVA_CONSECUTIVE_REPEAT:
			case PRIM_SVA_NON_CONSECUTIVE_REPEAT: case PRIM_SVA_GOTO_REPEAT:
			case PRIM_SVA_MATCH_ITEM_TRIGGER: case PRIM_SVA_AND: case PRIM_SVA_OR:
			case PRIM_SVA_SEQ_AND: case PRIM_SVA_SEQ_OR: case PRIM_SVA_EVENT_OR:
			case PRIM_SVA_OVERLAPPED_IMPLICATION: case PRIM_SVA_NON_OVERLAPPED_IMPLICATION:
			case PRIM_SVA_OVERLAPPED_FOLLOWED_BY: case PRIM_SVA_NON_OVERLAPPED_FOLLOWED_BY:
			case PRIM_SVA_INTERSECT: case PRIM_SVA_THROUGHOUT: case PRIM_SVA_WITHIN:
			case PRIM_SVA_AT: case PRIM_SVA_DISABLE_IFF: case PRIM_SVA_SAMPLED:
			case PRIM_SVA_ROSE: case PRIM_SVA_FELL: case PRIM_SVA_STABLE:
			case PRIM_SVA_PAST: case PRIM_SVA_MATCH_ITEM_ASSIGN: case PRIM_SVA_SEQ_CONCAT:
			case PRIM_SVA_IF: case PRIM_SVA_RESTRICT: case PRIM_SVA_TRIGGERED:
			case PRIM_SVA_STRONG: case PRIM_SVA_WEAK: case PRIM_SVA_NEXTTIME:
			case PRIM_SVA_S_NEXTTIME: case PRIM_SVA_ALWAYS: case PRIM_SVA_S_ALWAYS:
			case PRIM_SVA_S_EVENTUALLY: case PRIM_SVA_EVENTUALLY: case PRIM_SVA_UNTIL:
			case PRIM_SVA_S_UNTIL: case PRIM_SVA_UNTIL_WITH: case PRIM_SVA_S_UNTIL_WITH:
			case PRIM_SVA_IMPLIES: case PRIM_SVA_IFF: case PRIM_SVA_ACCEPT_ON:
			case PRIM_SVA_REJECT_ON: case PRIM_SVA_SYNC_ACCEPT_ON:
			case PRIM_SVA_SYNC_REJECT_ON: case PRIM_SVA_GLOBAL_CLOCKING_DEF:
			case PRIM_SVA_GLOBAL_CLOCKING_REF: case PRIM_SVA_IMMEDIATE_ASSUME:
			case PRIM_SVA_IMMEDIATE_COVER: case OPER_SVA_SAMPLED: case OPER_SVA_STABLE:
			// clang-format on
			{
				return ast.new_seq(AstUnhandled{stringf("unhandled case: parse_sequence: %s (%s) at %s:%d ", inst->Name(),
									inst->View()->Owner()->Name(), LineFile::GetFileName(inst->Linefile()),
									LineFile::GetLineNo(inst->Linefile()))});
			}
		default: {
			return ast.new_seq(AstBoolSeq{SigBit(importer->net_map_at(net))});
		} break;
		}
	}

	dict<Verific::Net *, AstPropId> parse_property_cache;

	AstPropId parse_property(Verific::Net *net)
	{
		auto it = parse_property_cache.find(net);
		if (it != parse_property_cache.end())
			return it->second;

		AstPropId result = parse_property_uncached(net);
		parse_property_cache[net] = result;
		return result;
	}

	AstPropId parse_property_uncached(Verific::Net *net)
	{
		using namespace Verific;
		log_assert(net);
		prim_type type = PRIM_END;
		Instance *inst = nullptr;
		if (!net->IsMultipleDriven() && (inst = net->Driver()))
			type = inst->Type();
		switch (type) {

		case PRIM_SVA_AT: {
			std::shared_ptr<VerificClocking> new_clocking(new VerificClocking(importer, net));
			return ast.new_prop(AstClockProp{new_clocking, parse_property(new_clocking->body_net)});
		} break;
		case PRIM_SVA_OVERLAPPED_IMPLICATION:
			return ast.new_prop(AstImplProp{AstImplProp::IMPL, AstImplProp::OVERLAPPING, parse_sequence(inst->GetInput1()),
							parse_property(inst->GetInput2())});
		case PRIM_SVA_NON_OVERLAPPED_IMPLICATION:
			return ast.new_prop(AstImplProp{AstImplProp::IMPL, AstImplProp::NONOVERLAPPING, parse_sequence(inst->GetInput1()),
							parse_property(inst->GetInput2())});
		case PRIM_SVA_OVERLAPPED_FOLLOWED_BY:
			return ast.new_prop(AstImplProp{AstImplProp::FOLLOWED_BY, AstImplProp::OVERLAPPING, parse_sequence(inst->GetInput1()),
							parse_property(inst->GetInput2())});
		case PRIM_SVA_NON_OVERLAPPED_FOLLOWED_BY:
			return ast.new_prop(AstImplProp{AstImplProp::FOLLOWED_BY, AstImplProp::NONOVERLAPPING, parse_sequence(inst->GetInput1()),
							parse_property(inst->GetInput2())});
		case PRIM_SVA_NOT:
			return ast.new_prop(AstNotProp{parse_property(inst->GetInput())});
		case PRIM_SVA_AND:
			return ast.new_prop(AstLogicProp{AstLogicProp::AND, {parse_property(inst->GetInput1()), parse_property(inst->GetInput2())}});
		case PRIM_SVA_OR:
			return ast.new_prop(AstLogicProp{AstLogicProp::OR, {parse_property(inst->GetInput1()), parse_property(inst->GetInput2())}});
		case PRIM_SVA_IMPLIES:
			return ast.new_prop(
			  AstLogicProp{AstLogicProp::IMPLIES, {parse_property(inst->GetInput1()), parse_property(inst->GetInput2())}});
		case PRIM_SVA_IFF:
			return ast.new_prop(AstLogicProp{AstLogicProp::IFF, {parse_property(inst->GetInput1()), parse_property(inst->GetInput2())}});
		case PRIM_SVA_NEXTTIME:
			return ast.new_prop(AstNexttimeProp{RepetitionRange::parse(inst).known_finite_constant(), parse_property(inst->GetInput())});

		default: {
			AstSeqId seq = parse_sequence(net);
			return ast.new_prop(AstSeqProp{seq});
		}
		}
	};
};

struct Fsm;

struct FsmStateId {
	int index = -1;

	FsmStateId() {}
	explicit FsmStateId(int index) : index(index) {}

	bool operator==(const FsmStateId &other) const { return index == other.index; }
	bool operator!=(const FsmStateId &other) const { return index != other.index; }
	[[nodiscard]] Hasher hash_into(Hasher h) const
	{
		h.eat(index);
		return h;
	}

	void format_into(std::string &target, Fsm *fsm = nullptr) const;
};

struct FsmNext {
	FsmStateId next;
	RepetitionRange delay;
	void format_into(std::string &target, Fsm *fsm = nullptr) const
	{
		target += "next ";
		delay.format_into(target);
		target += " ";
		next.format_into(target, fsm);
	}
	std::string dot_node_color() const { return "#eeeeee"; }
};

struct FsmBranch {
	SigBit bit;
	FsmStateId if_true;
	FsmStateId if_false;

	void format_into(std::string &target, Fsm *fsm = nullptr) const
	{
		target += log_signal(bit);
		target += " ? ";
		if_true.format_into(target, fsm);
		target += " : ";
		if_false.format_into(target, fsm);
	}
	std::string dot_node_color() const { return "#ffffaa"; }
};

struct FsmCombine {
	enum class Type {
		OR,
		AND,
	} type;
	static constexpr Type OR = Type::OR;
	static constexpr Type AND = Type::AND;
	pool<FsmStateId> states;

	void format_into(std::string &target, Fsm *fsm = nullptr) const
	{
		if (states.empty()) {
			target += type == AND ? "ACCEPT /* empty and */" : "REJECT /* empty or */";
			return;
		}
		bool first = true;
		for (auto state : states) {
			if (!first)
				target += type == AND ? " and " : " or ";
			first = false;
			state.format_into(target, fsm);
		}
	}
	std::string dot_node_color() const { return type == OR ? "#ffffff" : "#ffddcc"; }
};

struct FsmSeq {
	typedef AstImplProp::Type Type;
	AstImplProp::Type type;
	static constexpr Type IMPL = Type::IMPL;
	static constexpr Type FOLLOWED_BY = Type::FOLLOWED_BY;

	FsmStateId seq;
	FsmStateId then;

	void format_into(std::string &target, Fsm *fsm = nullptr) const
	{
		if (type == IMPL) {
			target += "forall ";
			seq.format_into(target, fsm);
			target += " require ";
			then.format_into(target, fsm);
		} else {
			target += "exists ";
			seq.format_into(target, fsm);
			target += " with ";
			then.format_into(target, fsm);
		}
	}
	std::string dot_node_color() const { return type == IMPL ? "#ffaaff" : "#aaffff"; }
};

struct FsmRepeat {
	FsmStateId seq;
	RepetitionRange count;

	void format_into(std::string &target, Fsm *fsm = nullptr) const
	{
		target += "repeat ";
		seq.format_into(target, fsm);
		target += " ";
		count.format_into(target);
	}
	std::string dot_node_color() const { return "#ccccff"; }
};

struct FsmSink {
	enum class Type {
		REJECT,
		ACCEPT,
	} type;
	static constexpr Type REJECT = Type::REJECT;
	static constexpr Type ACCEPT = Type::ACCEPT;

	void format_into(std::string &target, Fsm *fsm = nullptr) const
	{
		(void)fsm;
		target += type == ACCEPT ? "ACCEPT" : "REJECT";
	}
	std::string dot_node_color() const { return type == ACCEPT ? "#aaffaa" : "#ffcccc"; }
};

struct FsmUnhandled {
	std::string message;
	std::array<FsmStateId, 2> next = {};

	void format_into(std::string &target, Fsm *fsm = nullptr) const;
	std::string dot_node_color() const { return "#ff8888"; }
};

typedef std::variant<EmptyVariant, FsmUnhandled, FsmSink, FsmNext, FsmBranch, FsmCombine, FsmRepeat, FsmSeq> FsmStateVariant;

struct FsmState : public FsmStateVariant {
	using FsmStateVariant::variant;

	FsmState() : FsmStateVariant::variant(EmptyVariant()) {}

	bool empty() const { return std::holds_alternative<EmptyVariant>(*this); }
	template <class T>
	bool is() const
	{
		return std::holds_alternative<T>(*this);
	}
	template <class T>
	T const &as() const
	{
		log_assert(std::holds_alternative<T>(*this));
		return std::get<T>(*this);
	}
	template <class T>
	T &as()
	{
		log_assert(std::holds_alternative<T>(*this));
		return std::get<T>(*this);
	}

	void format_into(std::string &target, Fsm *fsm = nullptr) const
	{
		std::visit([&](auto &&arg) { arg.format_into(target, fsm); }, *this);
	}

	std::string dot_node_color() const
	{
		return std::visit([&](auto &&arg) { return arg.dot_node_color(); }, *this);
	}
};

struct Fsm {
	StableVec<FsmState> states;

	FsmState const &operator[](FsmStateId id) const
	{
		log_assert(defined(id));
		return states[id.index];
	}
	FsmState &operator[](FsmStateId id)
	{
		log_assert(defined(id));
		return states[id.index];
	}

	bool defined(FsmStateId id) const { return 0 <= id.index && id.index < GetSize(states) && !states[id.index].empty(); }

	template <class... T>
	FsmStateId new_state(T &&...args)
	{
		FsmStateId new_node(GetSize(states));
		states.emplace_back(std::forward<decltype(args)>(args)...);
		return new_node;
	}

	void format_into(std::string &target) const
	{
		for (int i = 0; i < GetSize(states); ++i) {
			if (states[i].empty())
				continue;
			FsmStateId(i).format_into(target);
			target += " = ";
			states[i].format_into(target);
			target += '\n';
		}
	}

	bool dot_mode = false;
	std::vector<FsmStateId> dot_links;

	void write_dot_into(std::string const &path, std::optional<FsmStateId> init = std::nullopt)
	{
		std::ofstream out(path);
		out << "digraph {\n";
		out << "  node [shape=rect, style=\"rounded,filled\", margin=0];\n";
		dot_mode = true;

		for (int i = 0; i < GetSize(states); ++i) {
			if (states[i].empty())
				continue;
			dot_links.clear();

			std::string node_name = format(FsmStateId(i));
			std::string label = dot_escape_html(node_name + "\n") + dot_escape_with_ports(format(states[i], this));

			out << stringf("  %s [label=<%s>, fillcolor=\"%s\"];\n", node_name.c_str(), dot_table_box(label, false).c_str(),
				       states[i].dot_node_color().c_str());

			int link_nr = 0;
			for (auto dot_link : dot_links) {
				std::string link_name = format(dot_link);
				out << stringf("  %s:c%d -> %s;\n", node_name.c_str(), link_nr++, link_name.c_str());
			}
		}

		if (init.has_value()) {
			out << "  init [label=<init>,shape=plain,style=solid];\n";
			out << stringf("  init -> %s;\n", format(init.value()).c_str());
		}

		dot_mode = false;
		out << "}\n";
	}
};

void FsmUnhandled::format_into(std::string &target, Fsm *fsm) const
{
	if (fsm && fsm->dot_mode) {
		bool rhs = false;
		for (auto c : message)
			if ((rhs |= (c == '=')) || c != ' ' || !(target.empty() || target.back() == '\n'))
				target += c == ':' ? '\n' : c;
		target += '\n';
		for (auto next_state : next) {
			if (next_state.index < 0)
				continue;
			target += ' ';
			next_state.format_into(target, fsm);
		}
		if (target.back() == '\n')
			target.pop_back();
	} else {
		target += message;
	}
}

void FsmStateId::format_into(std::string &target, Fsm *fsm) const
{
	if (index < 0) {
		target += "???";
	} else if (!fsm) {
		target += stringf("q%d", index);
	} else if (fsm->dot_mode) {
		target += stringf("\x01q%d\x02", index);
		fsm->dot_links.push_back(*this);
	} else {
		target += stringf("( /* q%d */ ", index);
		if (fsm->defined(*this)) {
			(*fsm)[*this].format_into(target, fsm);
		} else {
			target += "???";
		}
		target += ')';
	}
}

struct AstToFsmWorker {
	Ast ast;
	AstPropId ast_root;
	Fsm fsm;
	FsmStateId fsm_init;

	dict<AstPropId, FsmStateId> convert_property_cache;
	dict<AstSeqId, FsmStateId> convert_sequence_cache;

	FsmStateId convert_property(AstPropId prop)
	{
		auto found = convert_property_cache.find(prop);
		if (found != convert_property_cache.end())
			return found->second;
		auto computed = std::visit([&](auto &&arg) { return convert_property(arg, prop); }, ast[prop]);
		convert_property_cache[prop] = computed;
		return computed;
	}

	FsmStateId convert_property(AstClockProp const &prop, AstPropId prop_id)
	{
		(void)prop_id;
		log_warning("clocking not handled nor checked!\n");

		return convert_property(prop.prop);
	}

	FsmStateId convert_property(AstImplProp const &prop, AstPropId prop_id)
	{
		if (prop.overlap != AstImplProp::OVERLAPPING)
			return convert_property(prop, prop_id, std::nullopt);

		FsmStateId then_prop = convert_property(prop.prop);
		FsmStateId match_seq = convert_sequence(prop.seq);

		return fsm.new_state(FsmSeq{prop.type, match_seq, then_prop});
	}

	FsmStateId convert_property(AstBoolProp const &prop, AstPropId prop_id)
	{
		if (prop.is_constant())
			return fsm.new_state(FsmSink{prop.constant_value() ? FsmSink::ACCEPT : FsmSink::REJECT});

		FsmStateId accept = fsm.new_state(FsmSink{FsmSink::ACCEPT});
		FsmStateId reject = fsm.new_state(FsmSink{FsmSink::REJECT});

		if (!prop.value)
			std::swap(accept, reject);

		return fsm.new_state(FsmBranch{prop.atom, accept, reject});
	}

	FsmStateId convert_property(AstNexttimeProp const &prop, AstPropId prop_id)
	{
		return fsm.new_state(FsmNext{convert_property(prop.prop), prop.ticks});
	}

	FsmStateId convert_property(AstLogicProp const &prop, AstPropId prop_id)
	{
		switch (prop.type) {
		case AstLogicProp::IMPLIES:
		case AstLogicProp::IFF: {
			return convert_property(prop, prop_id, std::nullopt);
		}
		case AstLogicProp::AND:
			return fsm.new_state(FsmCombine{FsmCombine::AND, {{convert_property(prop.prop[0]), convert_property(prop.prop[1])}}});
		case AstLogicProp::OR:
			return fsm.new_state(FsmCombine{FsmCombine::OR, {{convert_property(prop.prop[0]), convert_property(prop.prop[1])}}});
		}
	}

	template <class T>
	FsmStateId convert_property(T const &prop, AstPropId prop_id, std::nullopt_t force_fallback = std::nullopt)
	{
		(void)force_fallback;
		AstPropId lowered_prop = ast.lower(prop_id);
		if (lowered_prop != prop_id)
			return convert_property(lowered_prop);
		std::string message = stringf("unhandled variant: convert_property: %s = %s", format(prop_id).c_str(), format(prop).c_str());
		log_warning("%s\n", message.c_str());
		auto unhandled = FsmUnhandled{message};
		return fsm.new_state(unhandled);
	}

	FsmStateId convert_sequence(AstSeqId seq)
	{
		// TODO simplify modulo changes in empty admittance

		auto found = convert_sequence_cache.find(seq);
		if (found != convert_sequence_cache.end())
			return found->second;
		auto computed = std::visit([&](auto &&arg) { return convert_sequence(arg, seq); }, ast[seq]);
		convert_sequence_cache[seq] = computed;
		return computed;
	}

	FsmStateId convert_sequence(AstConcatSeq const &seq, AstSeqId seq_id)
	{
		auto [prefix, suffix] = seq.seqs;
		bool prefix_admits_empty = ast.admits_empty(prefix);
		bool suffix_admits_empty = ast.admits_empty(suffix);

		FsmStateId suffix_init = convert_sequence(suffix);

		FsmStateId prefix_init = convert_sequence(prefix);

		FsmStateId delayed_suffix = fsm.new_state(FsmNext{suffix_init, seq.delay});

		FsmStateId concatenation = fsm.new_state(FsmSeq{FsmSeq::FOLLOWED_BY, prefix_init, delayed_suffix});

		if (seq.delay.max > 0 && (prefix_admits_empty || suffix_admits_empty)) {
			pool<FsmStateId> choices = {concatenation};
			if (prefix_admits_empty)
				choices.insert(fsm.new_state(FsmNext{suffix_init, seq.delay - 1}));
			if (suffix_admits_empty) {
				FsmStateId suffix_delay = fsm.new_state(FsmNext{fsm.new_state(FsmSink{FsmSink::ACCEPT}), seq.delay - 1});
				choices.insert(fsm.new_state(FsmSeq{FsmSeq::FOLLOWED_BY, prefix_init, suffix_delay}));
			}
			return fsm.new_state(FsmCombine{FsmCombine::OR, choices});
		}

		return concatenation;
	}

	FsmStateId convert_sequence(AstDelaySeq const &seq, AstSeqId seq_id)
	{
		bool suffix_admits_empty = ast.admits_empty(seq.seq);

		FsmStateId suffix_init = convert_sequence(seq.seq);
		if (seq.delay.max == 0)
			return suffix_init;

		FsmStateId delayed_suffix = fsm.new_state(FsmNext{suffix_init, seq.delay});

		if (suffix_admits_empty) {
			FsmStateId delay = fsm.new_state(FsmNext{fsm.new_state(FsmSink{FsmSink::ACCEPT}), seq.delay - 1});
			return fsm.new_state(FsmCombine{FsmCombine::OR, {{delayed_suffix, delay}}});
		}
		return delayed_suffix;
	}

	FsmStateId convert_sequence(AstBoolSeq const &seq, AstSeqId seq_id)
	{
		FsmStateId accept = fsm.new_state(FsmSink{FsmSink::ACCEPT});
		FsmStateId reject = fsm.new_state(FsmSink{FsmSink::REJECT});
		if (seq.is_constant())
			return seq.constant_value() ? accept : reject;

		if (!seq.value)
			std::swap(accept, reject);

		return fsm.new_state(FsmBranch{seq.atom, accept, reject});
	}

	FsmStateId convert_sequence(AstRepeatSeq const &seq, AstSeqId seq_id)
	{
		switch (seq.type) {
		case AstRepeatSeq::NONCONSECUTIVE:
		case AstRepeatSeq::GOTO:
		case AstRepeatSeq::CONSECUTIVE:
			return convert_sequence(seq, seq_id, std::nullopt);
		case AstRepeatSeq::OVERLAPPING: {
			return fsm.new_state(FsmRepeat{convert_sequence(seq.seq), seq.repeats});
		}
		}
	}

	FsmStateId convert_sequence(AstBinaryOpSeq const &seq, AstSeqId seq_id)
	{
		switch (seq.type) {
		case AstBinaryOpSeq::AND:
		case AstBinaryOpSeq::THROUGHOUT:
		case AstBinaryOpSeq::WITHIN:
			return convert_sequence(seq, seq_id, std::nullopt); // TODO
		case AstBinaryOpSeq::INTERSECT:
			return fsm.new_state(FsmCombine{FsmCombine::AND, {{convert_sequence(seq.seqs[0]), convert_sequence(seq.seqs[1])}}});
		case AstBinaryOpSeq::OR:
			return fsm.new_state(FsmCombine{FsmCombine::OR, {{convert_sequence(seq.seqs[0]), convert_sequence(seq.seqs[1])}}});
		}
	}

	template <class T>
	FsmStateId convert_sequence(T const &seq, AstSeqId seq_id, std::nullopt_t force_fallback = std::nullopt)
	{
		(void)force_fallback;
		AstSeqId lowered_seq = ast.lower(seq_id);
		if (lowered_seq != seq_id)
			return convert_sequence(lowered_seq);

		std::string message = stringf("unhandled variant: convert_sequence: %s = %s", format(seq_id).c_str(), format(seq).c_str());
		log_warning("%s\n", message.c_str());
		auto unhandled = FsmUnhandled{message};
		return fsm.new_state(unhandled);
	}

	void convert() { fsm_init = convert_property(ast_root); }
};

void verific_import_new_impl(VerificImporter *importer, Verific::Instance *inst)
{
	VerificToAstWorker worker;
	worker.importer = importer;
	worker.root = inst;
	worker.import();

	std::string s;
	worker.ast.format_into(s);
	log("%s\n", s.c_str());

	s.clear();
	worker.ast_root.format_into(s, &worker.ast);
	log("%s\n", s.c_str());

	worker.ast_root = worker.ast.new_prop(AstNotProp{worker.ast_root});

	AstToFsmWorker worker2;
	worker2.ast = std::move(worker.ast);
	worker2.ast_root = worker.ast_root;

	worker2.convert();

	worker2.ast.write_dot_into("/tmp/pos.dot", worker2.ast_root);

	s.clear();
	worker2.fsm.format_into(s);
	log("%s\n", s.c_str());

	s.clear();
	worker2.fsm_init.format_into(s, &worker2.fsm);
	log("%s\n", s.c_str());
	worker2.fsm.write_dot_into("/tmp/neg.dot", worker2.fsm_init);

	log("done\n");
}

PRIVATE_NAMESPACE_END

YOSYS_NAMESPACE_BEGIN

void verific_import_new(VerificImporter *importer, Verific::Instance *inst) { verific_import_new_impl(importer, inst); }

YOSYS_NAMESPACE_END
