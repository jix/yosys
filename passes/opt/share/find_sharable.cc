/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2025  Jannis Harder <jix@yosyshq.com> <me@jix.one>
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

#include "passes/opt/share/find_sharable.h"
#include "libs/cadical/src/cadical.hpp"
#include <algorithm>
#include <chrono>
#include <variant>

USING_YOSYS_NAMESPACE

PRIVATE_NAMESPACE_BEGIN

using std::vector;
typedef int lit;

constexpr int SAT = CaDiCaL::SATISFIABLE;
constexpr int UNSAT = CaDiCaL::UNSATISFIABLE;
constexpr int UNKNOWN = CaDiCaL::UNKNOWN;

template <class T>
struct ClauseIterator : public CaDiCaL::ClauseIterator, CaDiCaL::WitnessIterator {
	T callback;
	ClauseIterator(T &&callback) : callback(std::move(callback)) {}

	virtual bool clause(const vector<int> &clause) override { return this->callback(clause); }
	virtual bool witness(const vector<int> &clause, const vector<int> &, uint64_t = 0) override { return this->callback(clause); }
};

struct Solver : public CaDiCaL::Solver {
	int cached_false = 0;

	int fresh_var()
	{
		int new_var = vars() + 1;
		reserve(new_var);
		return new_var;
	}

	template <typename F>
	void traverse_clauses(F &&callback)
	{
		ClauseIterator iter(std::move(callback));
		CaDiCaL::Solver::traverse_clauses(iter);
	}

	void add(int lit) { CaDiCaL::Solver::add(lit); }

	void add(std::initializer_list<int> lits)
	{
		for (int lit : lits)
			add(lit);
		add(0);
	}

	bool is_true(int lit) { return val(lit) == lit; }

	lit make_or(std::initializer_list<lit> lits) { return make_or(lits.begin(), lits.end()); }

	template <typename T>
	lit make_or(T a, T b)
	{
		switch (b - a) {
		case 0:
			return make_false();
		case 1:
			return a[0];
		default:

			lit result = fresh_var();
			for (T i = a; i != b; ++i)
				add(*i);
			add(-result);
			add(0);

			for (T i = a; i != b; ++i)
				add({-*i, result});

			return result;
		}
	}

	lit make_false()
	{
		if (!cached_false) {
			cached_false = fresh_var();
			add({-cached_false});
		}
		return cached_false;
	}

	void at_most_one(const vector<lit> &vec) { at_most_one(vec.begin(), vec.end()); }
	void at_most_one(const vector<lit> &vec, lit if_true) { at_most_one(vec.begin(), vec.end(), if_true); }

	template <typename T>
	void at_most_one(T a, T b)
	{
		size_t n = b - a;
		if (n < 8) {
			for (size_t i = 1; i < n; i++)
				for (size_t j = 0; j < i; j++)
					add({-a[i], -a[j]});
		} else {
			size_t size = 1;

			while (size <= n) {
				lit sel = fresh_var();
				size_t i = 0;
				for (auto it = a; it != b; ++it) {
					bool pol = (i++) & size;
					add({-*it, pol ? -sel : sel});
				}
				size <<= 1;
			}
		}
	}

	template <typename T>
	void at_most_one(T a, T b, lit if_true)
	{
		size_t n = b - a;
		if (n < 8) {
			for (size_t i = 1; i < n; i++)
				for (size_t j = 0; j < i; j++)
					add({-if_true, -a[i], -a[j]});
		} else {
			size_t size = 1;

			while (size <= n) {
				lit sel_pos = fresh_var();
				lit sel_neg = fresh_var();
				add({-if_true, -sel_pos, -sel_neg});
				size_t i = 0;
				for (auto it = a; it != b; ++it) {
					bool pol = (i++) & size;
					add({-*it, pol ? sel_pos : sel_neg});
				}
				size <<= 1;
			}
		}
	}
};

struct SimpleCliqueFinder {
private:
	dict<lit, lit> internal_from_external;
	dict<lit, lit> external_from_internal;

	lit last_inode = 0;

	pool<lit> clique_candidates;
	pool<lit> delayed_candidates;
	std::vector<lit> found_clique;
	pool<lit> failed;

	pool<lit> found_clique_enodes;
	std::vector<lit> tmp_inodes;

	struct node {
		bool exhausted = false;
		pool<lit> observed;
		pool<lit> excluded;

		pool<lit> &edges(bool enforce_weak) { return enforce_weak ? observed : excluded; }
	};

	dict<lit, node> nodes;
	pool<lit> exhausted_enodes;

public:
	vector<std::array<lit, 2>> exhausted_enode_exclusions;

	SimpleCliqueFinder() {}

	void add_node(lit enode)
	{
		lit inode = ++last_inode;
		auto [_, inserted] = internal_from_external.emplace(enode, inode);
		log_assert(inserted);

		external_from_internal.emplace(inode, enode);

		nodes.emplace(inode, {});
	}

	void remove_node(lit enode)
	{
		if (exhausted_enodes.erase(enode))
			return;

		auto found = internal_from_external.find(enode);
		log_assert(found != internal_from_external.end());
		lit inode = found->second;
		external_from_internal.erase(inode);
		internal_from_external.erase(found);

		node &data = nodes[inode];
		for (lit x : data.observed)
			nodes[x].observed.erase(inode);
		for (lit y : data.excluded)
			nodes[y].excluded.erase(inode);
		nodes.erase(inode);
	}

	int pending_edges()
	{
		int pending = 0;

		for (auto const &[_, data] : nodes) {
			pending += GetSize(nodes) - 1 - GetSize(data.observed);
		}

		return pending / 2;
	}

	int exclusion_edges()
	{
		int excluded = 0;

		for (auto const &[_, data] : nodes) {
			excluded += GetSize(data.excluded);
		}

		return excluded / 2 + GetSize(exhausted_enode_exclusions);
	}

	int pending_nodes() { return GetSize(nodes); }

	const pool<lit> &find_clique(bool reduce_smallest = false)
	{
		found_clique_enodes.clear();
		found_clique.clear();
		clique_candidates.clear();
		delayed_candidates.clear();

		for (auto [x, _] : nodes)
			clique_candidates.insert(x);

		bool enforce_weak = true;

		while (true) {
			clique_candidates.sort([&](lit a, lit b) {
				size_t edges_a = nodes[a].observed.size();
				size_t edges_b = nodes[b].observed.size();
				if (edges_a != edges_b)
					return edges_a < edges_b;
				edges_a = nodes[a].excluded.size();
				edges_b = nodes[b].excluded.size();
				if (edges_a != edges_b)
					return edges_a < edges_b;
				return a < b;
			});

			while (!clique_candidates.empty()) {

				auto x_it = reduce_smallest ? clique_candidates.element(clique_candidates.size() - 1) : clique_candidates.begin();
				reduce_smallest = false;

				lit x = *x_it;
				int size = GetSize(nodes[x].edges(enforce_weak));
				// log("%d: %d : %d\n", x, size, GetSize(nodes) - size);
				clique_candidates.erase(x_it);

				if (size == GetSize(nodes) - 1) {
					log_abort();
					continue;
				}

				found_clique.push_back(x);

				auto const &edges = nodes[x].edges(enforce_weak);

				auto const &overlap = remove_overlap(clique_candidates, edges);
				if (enforce_weak)
					delayed_candidates.insert(overlap.begin(), overlap.end());
			}

			if (!enforce_weak)
				break;

			enforce_weak = false;

			std::swap(clique_candidates, delayed_candidates);

			for (lit x : found_clique) {
				auto const &edges = nodes[x].excluded;

				remove_overlap(clique_candidates, edges);
			}
		}
		found_clique_enodes.clear();
		for (lit x : found_clique)
			found_clique_enodes.insert(external_from_internal.at(x));

		return found_clique_enodes;
	}

	pool<lit> overlap;
	const pool<lit> &remove_overlap(pool<lit> &a, const pool<lit> &b)
	{
		bool a_smaller = a.size() < b.size();
		const pool<lit> &smaller = a_smaller ? a : b;
		const pool<lit> &larger = a_smaller ? b : a;

		overlap.clear();

		for (lit x : smaller) {
			if (larger.count(x)) {
				overlap.insert(x);
			}
		}
		for (lit x : overlap)
			a.erase(x);
		return overlap;
	}

	void remove_clique(const pool<lit> clique, bool weak)
	{
		clique_candidates.clear();
		for (lit x : clique)
			if (!exhausted_enodes.count(x))
				clique_candidates.insert(internal_from_external.at(x));

		for (lit x : clique_candidates) {
			node &data = nodes[x];
			for (lit y : clique_candidates) {
				if (y != x) {
					data.observed.insert(y);
					if (!weak) {
						data.excluded.insert(y);
					}
				}
			}
		}

		for (lit x : clique_candidates) {

			node &data = nodes[x];
			if (data.observed.size() == nodes.size() - 1) {
				lit x_enode = external_from_internal.at(x);
				for (lit y : data.excluded) {
					lit y_enode = external_from_internal.at(y);
					exhausted_enode_exclusions.push_back({x_enode, y_enode});
				}
				remove_node(x_enode);
				exhausted_enodes.insert(x_enode);
			}
		}
	}
};

struct MergeTree {
private:
	dict<lit, std::pair<int, std::array<lit, 2>>> merges;

	pool<lit> expanded;

public:
	void clear() { merges.clear(); }

	int expanded_size(lit x)
	{
		auto found = merges.find(x);
		if (found == merges.end())
			return 1;
		return found->second.first;
	}

	void merge(lit merged, std::array<lit, 2> pair)
	{
		int size = expanded_size(pair[0]) + expanded_size(pair[1]);
		merges.emplace(merged, {size, pair});
	}

	const pool<lit> &expand_merged(lit x)
	{
		expanded.clear();
		recurse(x);
		log_assert(GetSize(expanded) == expanded_size(x));
		return expanded;
	};

private:
	void recurse(lit x)
	{
		auto found = merges.find(x);
		if (found == merges.end()) {
			expanded.insert(x);
		} else {
			for (lit y : found->second.second)
				recurse(y);
		}
	}
};

struct FindSharableWorker {

	FindSharable &data;
	FindSharableWorker(FindSharable &data) : data(data)
	{
		log_prefix = [] { return ""; };
	}

	typedef std::chrono::time_point<std::chrono::steady_clock> time_point;

	// time_point start;

	int global_counter = 0;
	std::string global_prefix;

	std::function<void()> log_prefix;

	void rlog(const char *format, ...) YS_ATTRIBUTE(format(printf, 2, 3))
	{
		log_prefix();
		va_list ap;
		va_start(ap, format);
		logv(format, ap);
		va_end(ap);
	}

	// clang-format off
#define rlog_debug(...) do { if (ys_debug(1)) rlog_debug_fn(__VA_ARGS__); } while (0)
	// clang-format on

	void rlog_debug_fn(const char *format, ...) YS_ATTRIBUTE(format(printf, 2, 3))
	{
		if (ys_debug(1)) {

			log_prefix();
			va_list ap;
			va_start(ap, format);
			logv(format, ap);
			va_end(ap);
		}
	}

	std::unique_ptr<Solver> sat;
	std::unique_ptr<Solver> autarky_sat;
	idict<lit, 1> compact_var;
	pool<lit> all_indicators;

	void compute()
	{
		uint64_t start = PerformanceTimer::query();

		rlog("simplifying...\n");
		pool<lit> all_orig_indicators;

		for (auto const &group : data.observed_groups)
			all_orig_indicators.insert(group.begin(), group.end());

		rlog("cells %d\n", GetSize(all_orig_indicators));

		for (auto const &group : data.observed_groups)
			rlog("  group %d\n", GetSize(group));

		sat.reset(new Solver);
		sat->set("report", 1);

		for (auto const &clause : data.cnf) {
			for (lit x : clause)
				sat->add(x);
			sat->add(0);
		}
		for (auto x : all_orig_indicators)
			sat->freeze(x);

		int sat_state = sat->simplify(1);
		log_assert(sat_state != UNSAT);

		auto compact_lit = [&](lit x) {
			lit var = compact_var(abs(x));
			return x < 0 ? -var : var;
		};

		for (auto x : all_orig_indicators)
			all_indicators.insert(compact_lit(x));

		std::unique_ptr<Solver> compact_sat(new Solver);
		compact_sat->set("report", 0);

		sat->traverse_clauses([&](const vector<int> &clause) {
			for (lit x : clause) {
				compact_sat->add(compact_lit(x));
			}
			compact_sat->add(0);
			return true;
		});

		sat = std::move(compact_sat);

		int current_group = -1;

		for (auto orig_group : data.observed_groups) {
			current_group++;
			if (orig_group.size() < 2)
				continue;
			global_prefix = "";
			pool<lit> group_indicators;
			for (lit orig_x : orig_group)
				group_indicators.insert(compact_lit(orig_x));

			SimpleCliqueFinder cf;
			MergeTree mt;

			for (lit x : group_indicators)
				cf.add_node(x);

			log_prefix = [&] {
				int nodes_pend = cf.pending_nodes();
				int nodes_total = GetSize(orig_group);
				int edges_pend = cf.pending_edges();
				int edges_excl = cf.exclusion_edges();
				int edges_total = nodes_total * (nodes_total - 1) / 2;
				double runtime = (PerformanceTimer::query() - start) * 1e-9;
				log("%7.2f [%d/%d] [nodes: pend %d total %d] [edges: pend %d excl %d total %d] ", //
				    runtime, current_group, GetSize(data.observed_groups), nodes_pend, nodes_total, edges_pend, edges_excl,
				    edges_total);
			};

			pool<lit> clique;
			pool<lit> failed;
			pool<lit> solution;
			std::vector<lit> temporarily_removed;
			dict<lit, std::pair<int, int>> removal_reason;

			std::vector<lit> removal_reason_lits;

			while (true) {
				clique = cf.find_clique(global_counter & 1);
				mt.clear();
				temporarily_removed.clear();
				if (clique.empty())
					break;
				global_counter++;

				int expanded_clique_size = GetSize(clique);
				dict<lit, std::array<lit, 2>> parents;

				bool relaxed = false;

				while (true) {
					rlog_debug("clique size: %d (expanded size %d)\n", GetSize(clique), expanded_clique_size);
					for (lit x : clique)
						sat->assume(x);

					if (sat->solve() == SAT) {
						if (relaxed && !temporarily_removed.empty()) {
							relaxed = false;

							int new_expanded_size = 0;
							// for (lit x : temporarily_removed) {
							int write = 0;
							for (int read = 0; read < GetSize(temporarily_removed); ++read) {
								lit x = temporarily_removed[read];
								auto [begin, end] = removal_reason[x];
								bool try_again = false;
								for (int i = begin; i != end; ++i) {
									if (!clique.count(removal_reason_lits[i])) {
										try_again = true;
										break;
									}
								}
								if (!try_again) {
									temporarily_removed[write++] = x;
									continue;
								}
								new_expanded_size += mt.expanded_size(x);
								clique.insert(x);
							}
							temporarily_removed.resize(write);
							expanded_clique_size += new_expanded_size;
							rlog("reloaded %d nodes (expanded size %d)\n", GetSize(temporarily_removed),
							     new_expanded_size);

							continue;
						}
						break;
					}

					failed.clear();
					for (auto lit : clique)
						if (sat->failed(lit))
							failed.insert(lit);

					switch (failed.size()) {
					case 0:
						log_abort();
					case 1: {
						lit x = *failed.element(0);
						rlog("unit conflict, permanently removing %d expanded nodes\n", mt.expanded_size(x));
						expanded_clique_size -= mt.expanded_size(x);
						for (lit y : mt.expand_merged(x)) {
							group_indicators.erase(y);
							cf.remove_node(y);
						}

						clique.erase(x);
					} break;
					case 2: {
						lit x = *failed.element(0);
						lit y = *failed.element(1);
						rlog(
						  "binary conflict, temporarily merging nodes %d (expanded size %d) and %d (expanded size %d)\n", //
						  x, mt.expanded_size(x), y, mt.expanded_size(y));

						lit z = sat->make_or({x, y});

						clique.erase(x);
						clique.erase(y);
						clique.insert(z);

						mt.merge(z, {x, y});
						// relaxed = true;
					} break;
					default:
						lit x = 0;
						int expanded_size = INT_MAX;

						for (lit y : failed) {
							int y_size = mt.expanded_size(y);
							if (y_size <= expanded_size) {
								x = y;
								expanded_size = y_size;
							}
						}
						expanded_clique_size -= mt.expanded_size(x);
						rlog_debug("conflict of length %d, temporarily removing %d expanded nodes\n", GetSize(failed),
							   mt.expanded_size(x));
						temporarily_removed.push_back(x);
						clique.erase(x);

						failed.erase(x);

						int begin = GetSize(removal_reason_lits);
						removal_reason_lits.insert(removal_reason_lits.end(), failed.begin(), failed.end());
						int end = GetSize(removal_reason_lits);
						removal_reason[x] = {begin, end};
					}
				}

				temporarily_removed.insert(temporarily_removed.end(), clique.begin(), clique.end());

				for (lit x : temporarily_removed) {
					if (mt.expanded_size(x) == 1)
						continue;
					rlog("removing mutual exclusion clique of size %d\n", mt.expanded_size(x));
					cf.remove_clique(mt.expand_merged(x), false);
				}

				solution.clear();
				for (lit x : group_indicators)
					// for (lit y : temporarily_removed)
					// 	for (lit x : mt.expand_merged(y))
					if (sat->is_true(x))
						solution.insert(x);
				rlog("removing solution clique of size %d\n", GetSize(solution));
				cf.remove_clique(solution, true);
			}
			global_prefix = stringf(", N: %d, E: %d, X: %d", cf.pending_nodes(), cf.pending_edges(), cf.exclusion_edges());
			rlog("finished group\n");
			for (auto pair : cf.exhausted_enode_exclusions) {
				sat->assume(pair[0]);
				sat->assume(pair[1]);
				int status = sat->solve();
				log_assert(status == UNSAT);
			}
			rlog("verified excluded edges\n");
		}
	}
};

PRIVATE_NAMESPACE_END

void FindSharable::compute() { FindSharableWorker(*this).compute(); }
