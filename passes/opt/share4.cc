/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
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

#include <iterator>
#include <memory>
#include <optional>

#include "kernel/ff.h"
#include "kernel/macc.h"
#include "kernel/modtools.h"
#include "kernel/qcsat.h"
#include "kernel/sigtools.h"
#include "kernel/utils.h"
#include "kernel/yosys.h"
#include "libs/ezsat/ezcadical.h"

#include "passes/opt/share/find_sharable.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ShareWorkerConfig {
	int limit;
	size_t pattern_limit;
	bool opt_force;
	bool opt_aggressive;
	bool opt_fast;
	pool<RTLIL::IdString> generic_uni_ops, generic_bin_ops, generic_cbin_ops, generic_other_ops;
};

typedef int lit;

struct ShareWorker {
	const ShareWorkerConfig config;
	RTLIL::Design *design;
	RTLIL::Module *module;

	CellTypes cell_level_ct, handled_ct, ff_ct;
	pool<RTLIL::IdString> generic_ops;

	ModWalker modwalker;
	FfInitVals ffinitvals;
	std::unique_ptr<QuickConeSat> qcsat;

	dict<SigBit, lit> bit_observed_cache;

	dict<Cell *, lit> cell_observed_cache;
	dict<Cell *, lit> cell_outputs_observed_cache;
	dict<std::pair<Cell *, IdString>, lit> port_observed_cache;

	ShareWorker(ShareWorkerConfig config, RTLIL::Design *design) : config(config), design(design), modwalker(design)
	{
		generic_ops.insert(config.generic_uni_ops.begin(), config.generic_uni_ops.end());
		generic_ops.insert(config.generic_bin_ops.begin(), config.generic_bin_ops.end());
		generic_ops.insert(config.generic_cbin_ops.begin(), config.generic_cbin_ops.end());
		generic_ops.insert(config.generic_other_ops.begin(), config.generic_other_ops.end());

		cell_level_ct.setup_internals_eval();
		handled_ct.setup_internals_eval();

		for (const IdString &type : {ID($mux), ID($bmux), ID($pmux), ID($bwmux)}) {
			cell_level_ct.cell_types.erase(type);
			// handled_ct.cell_types.emplace(type, combinational_ct.cell_types[type]);
		}

		ff_ct.setup_internals_ff();
		handled_ct.setup_internals_ff();
	}

	lit observed(SigBit bit)
	{
		modwalker.sigmap.apply(bit);
		return observed_sigmapped(bit);
	}

	lit observed_sigmapped(SigBit bit)
	{
		log_debug("%sentering %s\n", prefix.c_str(), log_signal(bit));
		if (ys_debug(1))
			prefix += "  ";

		auto cached = cached_observed(bit);
		if (cached.has_value()) {
			log_debug("%scached %d\n", prefix.c_str(), cached.value());

			if (ys_debug(1))
				prefix.resize(prefix.size() - 2);
			return cached.value();
		}

		// Inserting a temporary always-observed cache entry ensures correctness
		// and termination in the presence of cycles.
		bit_observed_cache.emplace(bit, ezSAT::CONST_TRUE);
		lit computed = compute_observed(bit);

#if 0
		qcsat->prepare();
        if (!qcsat->ez->solve(computed)) {
			log("simplified false\n");
            computed = ezSAT::CONST_FALSE;
        } else if (!qcsat->ez->solve(qcsat->ez->NOT(computed))) {
			log("simplified true\n");
            computed = ezSAT::CONST_TRUE;
        }
#endif
		bit_observed_cache[bit] = computed;
		log_debug("%scomputed %d\n", prefix.c_str(), computed);
		if (ys_debug(1))
			prefix.resize(prefix.size() - 2);
		return computed;
	}

	std::string prefix;
	lit observed(SigSpec sig)
	{

		if (sig.size() == 1)
			return observed(sig[0]);

		modwalker.sigmap.apply(sig);
		std::vector<lit> computed_bits;

		if (computed_bits.size() > 1)
			for (auto const &bit : sig)
				if (cached_observed(bit) == ezSAT::CONST_TRUE)
					return ezSAT::CONST_TRUE;

		for (auto const &bit : sig) {
			computed_bits.push_back(observed_sigmapped(bit));
			if (computed_bits.back() == ezSAT::CONST_TRUE)
				return ezSAT::CONST_TRUE;
		}

		return qcsat->ez->vec_reduce_or(computed_bits);
	}

	// Should only be called from `observed`
	lit compute_observed(SigBit bit)
	{
		if (modwalker.signal_outputs.count(bit))
			return ezSAT::CONST_TRUE;

		auto found_consumers = modwalker.signal_consumers.find(bit);
		if (found_consumers == modwalker.signal_consumers.end() || found_consumers->second.empty())
			return ezSAT::CONST_FALSE;
		auto const &consumers = found_consumers->second;

		std::vector<lit> computed_consumers;

		if (consumers.size() > 1)
			for (auto const &consumer : consumers)
				if (cached_observed(consumer) == ezSAT::CONST_TRUE)
					return ezSAT::CONST_TRUE;

		for (auto const &consumer : consumers) {
			computed_consumers.push_back(compute_observed(consumer));
			if (computed_consumers.back() == ezSAT::CONST_TRUE)
				return ezSAT::CONST_TRUE;
		}

		return qcsat->ez->vec_reduce_or(computed_consumers);
	}

	// Should only be called with an already sigmapped bit
	std::optional<lit> cached_observed(SigBit const &bit)
	{
		if (!bit.is_wire())
			return ezSAT::CONST_FALSE;
		auto found_cached = bit_observed_cache.find(bit);
		if (found_cached != bit_observed_cache.end())
			return found_cached->second;

		return std::nullopt;
	}

	std::optional<lit> cached_observed(ModWalker::PortBit const &bit)
	{
		auto found_cell = cell_observed_cache.find(bit.cell);
		if (found_cell != cell_observed_cache.end())
			return found_cell->second;
		auto found_port = port_observed_cache.find({bit.cell, bit.port});
		if (found_port != port_observed_cache.end())
			return found_port->second;
		// auto found_port_bit = port_bit_observed_cache.find(bit);
		// if (found_port_bit != port_bit_observed_cache.end())
		// 	return found_port_bit->second;
		return std::nullopt;
	}

	lit compute_observed(ModWalker::PortBit const &bit)
	{
		log_debug("%sport bit %s.%s[%d] (%s)\n", prefix.c_str(), log_id(bit.cell), log_id(bit.port), bit.offset, log_id(bit.cell->type));
		auto cached = cached_observed(bit);
		if (cached.has_value())
			return cached.value();

		auto cell = bit.cell;
		auto const &port = bit.port;

		if (!handled_ct.cell_known(cell->type))
			return ezSAT::CONST_TRUE;

		if (cell_level_ct.cell_known(cell->type)) {
			lit cell_observed = cell_outputs_observed(cell);
			cell_observed_cache.emplace(cell, cell_observed);
			return cell_observed;
		}

		if (ff_ct.cell_known(cell->type)) {
			if (port != ID(D))
				return ezSAT::CONST_TRUE;
			FfData ff_data(&ffinitvals, cell);
			lit d_observed = ezSAT::CONST_TRUE;
			if (ff_data.has_ce) {
				lit ce_active = qcsat->importSigBit(ff_data.sig_ce);
				if (!ff_data.pol_ce)
					ce_active = qcsat->ez->NOT(ce_active);
				d_observed = ce_active;
			}
			if (ff_data.has_srst) {
				lit srst_inactive = qcsat->importSigBit(ff_data.sig_srst);
				if (ff_data.pol_srst)
					srst_inactive = qcsat->ez->NOT(srst_inactive);
				d_observed = qcsat->ez->AND(d_observed, srst_inactive);
			}
			port_observed_cache.emplace({cell, port}, d_observed);
			return d_observed;
		}

		if (cell->type.in(ID($mux), ID($pmux), ID($bmux), ID($bwmux))) {
			if (port == ID(S))
				return cell_outputs_observed(cell);

			const auto &sig_s = cell->getPort(ID(S));
			const auto &sig_y = cell->getPort(ID(Y));
			int offset;
			lit selected;
			vector<lit> s_value = qcsat->importSig(sig_s);
			if (cell->type == ID($bmux)) {
				int index = bit.offset / sig_y.size();
				offset = bit.offset % sig_y.size();

				for (int i = 0; i != GetSize(s_value); ++i) {
					if ((index & (1 << i)) == 0)
						s_value[i] = qcsat->ez->NOT(s_value[i]);
				}
				selected = qcsat->ez->vec_reduce_and(s_value);
			} else if (port == ID(A)) {
				selected =
				  cell->type == ID($bwmux) ? qcsat->ez->NOT(s_value[bit.offset]) : qcsat->ez->NOT(qcsat->ez->vec_reduce_or(s_value));
				offset = bit.offset;
			} else {
				int index = bit.offset;
				offset = bit.offset;
				if (cell->type == ID($mux)) {
					index = 0;
				} else if (cell->type == ID($pmux)) {
					index /= sig_y.size();
					offset %= sig_y.size();
				}
				selected = s_value[index];
			}

			lit y_observed = observed(sig_y[offset]);
			return qcsat->ez->AND(selected, y_observed);
		}

		log_abort();
	}

	lit cell_outputs_observed(Cell *cell)
	{
		auto found_cached = cell_outputs_observed_cache.find(cell);
		if (found_cached != cell_outputs_observed_cache.end())
			return found_cached->second;
		SigSpec all_outputs;
		for (auto const &[port, sig] : cell->connections())
			if (cell->output(port))
				all_outputs.append(sig);

		lit result = observed(all_outputs);
		cell_outputs_observed_cache.emplace(cell, result);
		return result;
	}

	idict<SigBit> always_observed;
	void mark_always_observed()
	{
		pool<Cell *> always_observed_cells;
		for (auto const &port : module->ports) {
			auto wire = module->wire(port);
			if (!wire->port_output)
				continue;
			SigSpec wire_sig = modwalker.sigmap(SigSpec(wire));
			for (auto const &bit : wire_sig)
				always_observed(bit);
		}

		for (auto cell : module->cells()) {
			if (ff_ct.cell_known(cell->type)) {
				FfData ff_data(&ffinitvals, cell);
				if (ff_data.has_ce)
					always_observed(modwalker.sigmap(SigBit(ff_data.sig_ce)));
				if (ff_data.has_srst)
					always_observed(modwalker.sigmap(SigBit(ff_data.has_srst)));
				if (ff_data.has_ce || ff_data.has_srst)
					continue;

				for (auto const &bit : modwalker.sigmap(ff_data.sig_d))
					always_observed(bit);
				continue;
			}

			if (handled_ct.cell_known(cell->type))
				continue;

			always_observed_cells.insert(cell);
			for (auto const &[port, sig] : cell->connections())
				if (cell->input(port))
					for (auto const &bit : modwalker.sigmap(sig))
						always_observed(bit);
		}

		for (int i = 0; i != GetSize(always_observed); i++) {
			SigBit current = always_observed[i];

			auto found_drivers = modwalker.signal_drivers.find(current);
			if (found_drivers == modwalker.signal_consumers.end() || found_drivers->second.empty())
				continue;
			for (auto const &driver_port_bit : found_drivers->second) {
				auto cell = driver_port_bit.cell;

				if (!cell_level_ct.cell_known(cell->type))
					continue;

				if (always_observed_cells.count(cell))
					continue;
				always_observed_cells.insert(cell);
				for (auto const &[port, sig] : cell->connections())
					if (cell->input(port))
						for (auto const &bit : modwalker.sigmap(sig))
							always_observed(bit);
			}
		}

		for (auto observed : always_observed) {
			bit_observed_cache[observed] = ezSAT::CONST_TRUE;
		}
	}

	void reset_for_new_module(RTLIL::Module *module)
	{
		this->module = module;
		modwalker.setup(module);
		ffinitvals.set(&modwalker.sigmap, module);
		shareable_cells.clear();
		shareable_cells_by_type.clear();
		reset_qcsat();
	}

	void reset_qcsat()
	{
		qcsat.reset(new QuickConeSat(modwalker, ezSatPtr(new ezCadical)));
		bit_observed_cache.clear();
		port_observed_cache.clear();
		cell_observed_cache.clear();
		cell_outputs_observed_cache.clear();
	}

	void find_equivalent_activations(dict<lit, std::vector<Cell *>> &cells)
	{
		cells[ezSAT::CONST_FALSE];
		cells[ezSAT::CONST_TRUE];
		std::vector<std::pair<int, lit>> active;

		for (auto const &[activation_lit, _] : cells)
			active.push_back({0, activation_lit});

		std::vector<lit> expr_vec;
		std::vector<lit> assumption_vec;
		std::vector<bool> model_vec;

		expr_vec.clear();
		for (auto const &[_, lit] : active)
			expr_vec.push_back(lit);
		// expr_vec.insert(expr_vec.begin(), active.begin(), active.end());

		bool solved = qcsat->ez->solve(expr_vec, model_vec);
		log_assert(solved);

		auto log_active = [&] {
			log("active:");
			int last_class = -2;
			for (int i = 0; i < GetSize(active); i++) {
				if (active[i].first != last_class) {
					last_class = active[i].first;
					log("\n  %3d:", last_class);
				}
				log(" %d[", active[i].second);
				auto sep = "";
				for (auto cell : cells[active[i].second]) {
					log("%s%p (%s)", sep, cell, log_id(cell->type));
					sep = ", ";
				}
				log("]");
			}
			log("\n");
		};

		while (!active.empty()) {
			log_active();

			for (int i = 0; i < GetSize(active); i++)
				active[i].first += GetSize(active) * int(model_vec[i]);

			std::stable_sort(active.begin(), active.end());

			log_active();

			int new_class = -1;
			int start_class = 0;
			int current_class = -1;
			int write = 0;
			std::vector<lit> constraints;
			std::vector<lit> positive;
			std::vector<lit> negative;
			auto add_lit = [&](lit x) {
				active[write++] = {new_class, x};
				positive.push_back(x);
				negative.push_back(qcsat->ez->NOT(x));
			};
			auto close_class = [&] {
				if (positive.empty())
					return;
				lit positive_lit = qcsat->ez->vec_reduce_or(positive);
				lit negative_lit = qcsat->ez->vec_reduce_or(negative);
				lit both = qcsat->ez->AND(positive_lit, negative_lit);
				constraints.push_back(both);
				positive.clear();
				negative.clear();
			};
			for (int read = 0; read < GetSize(active); read++) {
				if (current_class != active[read].first) {
					start_class = read;
					current_class = active[read].first;
					continue;
				}
				if (read == start_class + 1) {
					close_class();
					new_class++;
					add_lit(active[read - 1].second);
				}
				add_lit(active[read].second);
			}
			close_class();

			active.resize(write);

			lit assumption = qcsat->ez->vec_reduce_or(constraints);
			assumption_vec.clear();
			assumption_vec.push_back(assumption);

			expr_vec.clear();
			for (auto const &[_, lit] : active)
				expr_vec.push_back(lit);

			if (!qcsat->ez->solve(expr_vec, model_vec, assumption_vec))
				break;
		}
	}

	std::array<int, 2> weight2(ezSAT *sat, std::vector<int>::const_iterator begin, std::vector<int>::const_iterator end)
	{
		switch (end - begin) {
		case 0: {
			return {ezSAT::CONST_TRUE, ezSAT::CONST_FALSE};
		} break;
		case 1: {
			return {begin[0], ezSAT::CONST_FALSE};
		} break;
		case 2: {
			return {sat->OR(begin[0], begin[1]), sat->AND(begin[0], begin[1])};
		} break;
		default: {
			auto mid = begin + ((end - begin) / 2);
			auto [a1, a2] = weight2(sat, begin, mid);
			auto [b1, b2] = weight2(sat, mid, end);
			return {
			  sat->OR(a1, b1),
			  sat->OR(a2, sat->AND(a1, b1), b2),
			};
		}
		}
	}

	std::vector<std::vector<lit>> find_mutually_exclusive_sets(const dict<lit, Cell *> &cells)
	{
		pool<lit> active;
		for (auto const &[activation_lit, _] : cells)
			active.insert(activation_lit);

		dict<lit, std::vector<lit>> combined;
		std::vector<std::vector<lit>> result;

		auto take_combined = [&](lit x) {
			auto found = combined.find(x);
			if (found == combined.end())
				return std::vector<lit>{x};
			auto taken = std::move(found->second);
			combined.erase(found);
			return taken;
		};
		// static void combined(const dict<lit, std::pair<lit, lit>> tree)
		// std::vector<lit> never;

		pool<lit> assumptions(active);

		std::vector<lit> assumptions_vec;
		std::vector<lit> failed_vec;
		std::vector<lit> expr_vec;
		std::vector<bool> model_vec;

		std::vector<lit> skipped;

		// lit solution_finder = qcsat->ez->literal("solution finder");
		// lit solution_finder_neg = qcsat->ez->NOT(solution_finder);

		ezSatPtr candidate_solver;

		std::vector<lit> candidate_assumptions;
		std::vector<lit> candidate_expr;
		std::vector<bool> candidate_model;

		auto candidate_lit = [&](lit x) { return candidate_solver->frozen_literal(stringf("lit %d", x)); };

		std::vector<lit> required;
		dict<lit, int> removals;

		pool<std::pair<lit, lit>> blocked;

		auto restart = [&] {
			candidate_assumptions.clear();
			failed_vec.clear();
			for (auto lit : required) {
				failed_vec.push_back(candidate_lit(lit));
			}
			lit required_lit = candidate_solver->vec_reduce_or(failed_vec);
			candidate_solver->assume(required_lit);

			required.clear();

			assumptions.clear();
			assumptions.insert(active.begin(), active.end());
		};

		// pool<lit> failed;

		lit active_constraint = INT_MAX;

		pool<lit> preserve;

		while (true) {
			assumptions_vec.clear();
			assumptions_vec.insert(assumptions_vec.end(), assumptions.begin(), assumptions.end());

			// std::random_shuffle(assumptions_vec.begin(), assumptions_vec.end());
			expr_vec.clear();
			expr_vec.insert(expr_vec.begin(), active.begin(), active.end());

			if (qcsat->ez->solve(expr_vec, model_vec, assumptions_vec, failed_vec)) {
				if (assumptions.size() == active.size()) {
					log("fully active\n");
					break;
				}
				// models.push_back(model_vec);

				vector<lit> found;
				for (int i = 0; i < GetSize(model_vec); i++) {
					if (model_vec[i])
						found.push_back(candidate_lit(expr_vec[i]));
				}

				candidate_solver->assume(candidate_solver->onehot(found, true));

				if (active_constraint == INT_MAX) {

					candidate_expr.clear();
					for (lit x : expr_vec)
						candidate_expr.push_back(candidate_lit(x));
					auto [w1, w2] = weight2(&*candidate_solver, candidate_expr.begin(), candidate_expr.end());
					active_constraint = w2;
				}
				preserve.clear();

				candidate_assumptions.clear();
				candidate_assumptions.push_back(active_constraint);
				if (candidate_solver->solve(candidate_expr, candidate_model, candidate_assumptions)) {
					for (int i = 0; i < GetSize(active); i++) {
						if (candidate_model[i]) {
							preserve.insert(active[i]);
							if (preserve.size() == 2)
								break;
						}
					}

				} else {
					log("all blocked\n");
					break;
				}

				// twohot(candidate_solver, )

				// for (int i = 0; i < GetSize(found); i++) {
				// 	for (int j = i + 1; i < GetSize(found); i++) {
				// 		blocked.insert({found[i], found[j]});
				// 	}
				// }

				// double ratio = GetSize(blocked) / (double)(GetSize(active) * (GetSize(active) - 1) / 2);
				// log("ratio = %f\n", ratio);

				// if (GetSize(blocked) >= GetSize(active) * (GetSize(active) - 1) / 2) {
				// 	log("all blocked\n");
				// 	break;
				// }

				// // if (failed_vec.empty()) {
				// // 	log("exhausted after SAT\n");
				// // 	break;
				// // }
				log("restarting SAT\n");
				restart();
				continue;

				// failed_vec.clear();
				// failed_vec.push_back(solution_finder_neg);
				// for (int i = 0; i < GetSize(expr_vec); i++)
				// 	if (!model_vec[i])
				// 		failed_vec.push_back(expr_vec[i]);
				// qcsat->ez->assume(qcsat->ez->vec_reduce_or(failed_vec));

				// assumptions_vec.clear();
				// assumptions_vec.push_back(solution_finder);

				// if (qcsat->ez->solve(expr_vec, model_vec, assumptions_vec, failed_vec)) {
				// 	assumptions.clear();
				// 	for (int i = 0; i < GetSize(expr_vec); i++)
				// 		if (model_vec[i])
				// 			assumptions.insert(expr_vec[i]);
				// 	continue;
				// } else {
				// 	done = true;
				// 	break;
				// }
			}

			// failed.clear();
			// failed.insert(failed_vec.begin(), failed_vec.end());
			// if (failed.count(solution_finder)) {
			// 	assumptions_vec.pop_back();

			// 	if (qcsat->ez->solve(expr_vec, model_vec, assumptions_vec, failed_vec)) {
			// 	}
			// }

			log_assert(!failed_vec.empty());
			if (failed_vec.size() == 1) {
				log("never active %d\n", failed_vec[0]);
				active.erase(failed_vec[0]);
				assumptions.erase(failed_vec[0]);
				for (auto never : take_combined(failed_vec[1])) {
					result.push_back({never});
				}
			} else if (failed_vec.size() == 2) {
				lit either = qcsat->ez->OR(failed_vec[0], failed_vec[1]);

				lit either_candidate = candidate_solver->OR(candidate_lit(failed_vec[0]), candidate_lit(failed_vec[1]));
				candidate_solver->assume(candidate_solver->IFF(candidate_lit(either), either_candidate));

				// log("mutually exclusive %d vs %d => %d\n", failed_vec[0], failed_vec[1], either);
				active.erase(failed_vec[0]);
				active.erase(failed_vec[1]);
				assumptions.erase(failed_vec[0]);
				assumptions.erase(failed_vec[1]);
				active.insert(either);
				assumptions.insert(either);

				auto vec_0 = take_combined(failed_vec[0]);
				auto vec_1 = take_combined(failed_vec[1]);

				if (vec_1.size() > vec_0.size())
					std::swap(vec_0, vec_1);
				vec_0.insert(vec_0.end(), vec_1.begin(), vec_1.end());
				combined.emplace(either, std::move(vec_0));

				if (active_constraint != INT_MAX) {
					active_constraint = INT_MAX;
					// candidate_solver = ezSatPtr();
				}
			} else {
				// log("larger conflict %d\n", GetSize(failed_vec));
				bool good = false;

				// std::random_shuffle(failed_vec.begin(), failed_vec.end());

				std::stable_sort(failed_vec.begin(), failed_vec.end(), [&](const lit &a, const lit &b) {
					auto a_score = removals[a];
					auto b_score = removals[b];
					return b_score < a_score;
				});
				bool inner_good = false;
				for (lit x : failed_vec) {
					if (preserve.count(x))
						continue;
					assumptions.erase(x);
					removals[x] += 1;
					inner_good = true;
					break;
					// lit candidate_not_x = candidate_solver->NOT(candidate_lit(x));
					// candidate_assumptions.push_back(candidate_not_x);
					// // if (candidate_solver->solve(candidate_expr, candidate_model, candidate_assumptions)) {
					// // 	assumptions.erase(x);
					// // 	required.push_back(x);
					// // 	removals[x] += 1;
					// // 	good = true;
					// // 	break;
					// // }
					// candidate_assumptions.pop_back();
				}
				log_assert(inner_good);

				// if (!good) {
				// 	// if (required.empty()) {
				// 	// 	log("exhausted\n");
				// 	// 	break;
				// 	// }

				// 	log("restarting stuck\n");
				// 	restart();
				// }

				// assumptions.erase(failed_vec[0]);
				// required.push_back(failed_vec[0]);
			}
		}

		for (auto &item : combined)
			result.push_back(std::move(item.second));

		return result;
	}

	void operator()(RTLIL::Module *module)
	{
		reset_for_new_module(module);

		log("marking always observable cells\n");
		mark_always_observed();

		log("finding sharable cells\n");
		find_shareable_cells();

		// dict<lit, std::vector<Cell *>> cells;
		// for (auto cell : shareable_cells) {
		// 	lit cell_observed = cell_outputs_observed(cell);
		// 	if (cell_observed == ezSAT::CONST_TRUE)
		// 		continue;
		// 	if (cell_observed == ezSAT::CONST_FALSE) {
		// 		log("never observed %s (%s)\n", log_id(cell), log_id(cell->type));
		// 		continue;
		// 	}

		// 	cells[cell_observed].push_back(cell);
		// }

		// log("preparing cells\n");
		// qcsat->prepare();
		// log("solving...\n");

		// find_equivalent_activations(cells);

		int sharable_group = 0;

		// qcsat->ez->keep_cnf();

		FindSharable find_sharable;

		for (auto const &[type, by_type] : shareable_cells_by_type) {
			log("cell type %s\n", log_id(type));
			// 	// reset_for_new_cell_type();

			// std::vector<lit> cells_vec;
			// dict<lit, Cell *> cells;

			std::vector<lit> assume;
			pool<lit> observed_indicators;

			for (auto cell : by_type) {
				lit cell_observed = cell_outputs_observed(cell);

				if (cell_observed == ezSAT::CONST_TRUE)
					continue;
				if (cell_observed == ezSAT::CONST_FALSE) {
					log("never observed %s (%s)\n", log_id(cell), log_id(cell->type));
					cell->attributes[ID(never_observed)] = true;
					continue;
				}

				// qcsat->prepare();

				// assume.clear();
				// assume.push_back(cell_observed);

				// std::vector<bool> model;

				// if (!qcsat->ez->solve({}, model, assume)) {
				// 	log("never observed %s (%s) [sat]\n", log_id(cell), log_id(cell->type));
				// 	cell->attributes[ID(never_observed)] = true;
				// 	continue;
				// }

				// assume[0] = qcsat->ez->NOT(assume[0]);

				// if (!qcsat->ez->solve({}, model, assume)) {
				// 	log("always observed %s (%s) [sat]\n", log_id(cell), log_id(cell->type));
				// 	continue;
				// }

				// if (cells.count(cell_observed)) {
				// 	lit alias = qcsat->ez->frozen_literal(stringf("alias for %s", cell->name.c_str()));
				// 	qcsat->ez->assume(qcsat->ez->IFF(alias, cell_observed));
				// 	cell_observed = alias;
				// }
				// cells[cell_observed] = cell; //.emplace(cell_observed).push_back(cell);
				// cells_vec.push_back(cell_observed);
				observed_indicators.insert(qcsat->ez->bind(cell_observed));
			}
			// 	// for (auto wire : module->wires()) {
			// 	// 	if (!wire->name.isPublic())
			// 	// 		continue;
			// 	// 	for (auto const &bit : SigSpec(wire)) {
			// 	// 		inputs.push_back(bit);
			// 	// 		expressions.push_back(qcsat->importSigBit(bit));
			// 	// 	}
			// 	// }
			find_sharable.observed_groups.emplace_back(observed_indicators.begin(), observed_indicators.end());

			// log("preparing cells\n");

			// qcsat->prepare();

			// log("solving...\n");
			// std::vector<std::vector<lit>> cnf;

			// // qcsat->ez->getFullCnf(cnf);

			// auto const &cnf = qcsat->ez->cnf();

			// int var_count = 0;
			// for (auto const &clause : cnf)
			//     for (lit x : clause)
			//         var_count = max(var_count, abs(x));

			// log("p cnf %d %d\n", var_count, GetSize(cnf));

			// log("c target");

			// for (lit x : observed_indicators) {
			//     log(" %d", x);
			// }
			// log(" 0\n");

			// for (auto const &clause : cnf) {
			//     for (lit x : clause) {
			//         log("%d ", x);
			//     }
			//     log("0\n");
			// }
			// log("\n");
			// log("\n");

			// MutexSetCallbacks callbacks;
			// callbacks.any_of = [&](const std::vector<lit> &lits) { return qcsat->ez->vec_reduce_or(lits); };
			// callbacks.check = [&](const std::vector<lit> &get, std::vector<bool> &model, const std::vector<lit> &assumed,
			// 		      std::vector<lit> &failed) { return qcsat->ez->solve(get, model, assumed, failed); };
			// MutexSet ms(cells_vec, callbacks);
			// while (ms.step())
			// /* noop */;
			// for (auto activation_set : find_mutually_exclusive_sets(cells)) {
			// 	sharable_group++;
			// 	log("activation set\n");
			// 	for (auto activation_lit : activation_set) {
			// 		log("  activiation lit %d cell %s\n", activation_lit, log_id(cells[activation_lit]));
			// 		cells[activation_lit]->attributes[ID(sharable)] = sharable_group;
			// 		// for (auto cell : cells[activation_lit]) {
			// 		// 	log("    cell %s\n", log_id(cell));
			// 		// }
			// 	}
			// }
		}

		qcsat->prepare();
		qcsat->ez->consumeCnf(find_sharable.cnf);

		find_sharable.compute();

		// 	log("solving...\n");

		// 	lit a = qcsat->ez->literal(stringf("celltype %s", type.c_str()));
		// 	std::vector<int> assume = {qcsat->ez->NOT(a)};

		// 	find_equivalent_activations(cells);

		// 	for (auto activation_set : find_mutually_exclusive_sets(cells)) {
		// 		log("activation set\n");
		// 		for (auto activation_lit : activation_set) {
		// 			log("  activiation lit %d\n", activation_lit);
		// 			for (auto cell : cells[activation_lit]) {
		// 				log("    cell %s\n", log_id(cell));
		// 			}
		// 		}
		// 	}

		// 	// int counter = 0;
		// 	// std::vector<lit> new_clause;
		// 	// while (qcsat->ez->solve(model_expressions, model, assume)) {
		// 	// 	log("Found model %d\n", ++counter);

		// 	// 	new_clause.clear();
		// 	// 	new_clause.push_back(a);
		// 	// 	for (int i = 0; i < GetSize(cells); ++i) {
		// 	// 		bool is_observed = model[i];
		// 	// 		log("  cell %s (%s): %s\n", log_id(cells[i]), log_id(cells[i]->type), is_observed ? "observed" : "-");
		// 	// 		if (!is_observed)
		// 	// 			new_clause.push_back(model_expressions[i]);
		// 	// 	}
		// 	// 	// for (int i = 0; i < GetSize(inputs); ++i) {
		// 	// 	// 	bool value = model[i + GetSize(cells)];
		// 	// 	// 	log("  wire %s = %d\n", log_signal(inputs[i]), int(value));
		// 	// 	// }

		// 	// 	qcsat->ez->assume(qcsat->ez->vec_reduce_or(new_clause));

		// 	// 	if (counter)
		// 	// 		break;
		// 	// }

		// 	// mark_outputs_as_observed();
		// }
	}

	pool<RTLIL::Cell *> shareable_cells;
	dict<IdString, pool<RTLIL::Cell *>> shareable_cells_by_type;

	void find_shareable_cells()
	{
		for (auto cell : module->cells()) {
			if (!design->selected(module, cell) || !modwalker.ct.cell_known(cell->type))
				continue;

			for (auto &bit : modwalker.cell_outputs[cell])
				if (always_observed.count(bit))
					goto not_a_muxed_cell;

			if (0)
			not_a_muxed_cell:
				continue;

			if (config.opt_force) {
				shareable_cells.insert(cell);
				continue;
			}

			if (cell->type.in(ID($memrd), ID($memrd_v2))) {
				if (cell->parameters.at(ID::CLK_ENABLE).as_bool())
					continue;
				if (config.opt_aggressive || !modwalker.sigmap(cell->getPort(ID::ADDR)).is_fully_const())
					shareable_cells.insert(cell);
				continue;
			}

			if (cell->type.in(ID($mul), ID($div), ID($mod), ID($divfloor), ID($modfloor))) {
				if (config.opt_aggressive || cell->parameters.at(ID::Y_WIDTH).as_int() >= 4)
					shareable_cells.insert(cell);
				continue;
			}

			if (cell->type.in(ID($shl), ID($shr), ID($sshl), ID($sshr))) {
				if (config.opt_aggressive || cell->parameters.at(ID::Y_WIDTH).as_int() >= 8)
					shareable_cells.insert(cell);
				continue;
			}

			if (cell->type.in(ID($macc), ID($macc_v2))) {
				if (config.opt_aggressive || cell->parameters.at(ID::Y_WIDTH).as_int() >= 4)
					shareable_cells.insert(cell);
				continue;
			}

			if (generic_ops.count(cell->type)) {
				if (config.opt_aggressive)
					shareable_cells.insert(cell);
				continue;
			}
		}

		for (auto cell : shareable_cells) {
			shareable_cells_by_type[cell->type].insert(cell);
		}
	}
};

struct SharePass : public Pass {
	SharePass() : Pass("share4", "perform sat-based resource sharing") {}
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    share [options] [selection]\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		ShareWorkerConfig config;
		config.limit = -1;
		config.pattern_limit = design->scratchpad_get_int("share.pattern_limit", 1000);
		config.opt_force = false;
		config.opt_aggressive = false;
		config.opt_fast = false;

		config.generic_uni_ops.insert(ID($not));
		// config.generic_uni_ops.insert(ID($pos));
		config.generic_uni_ops.insert(ID($neg));

		config.generic_cbin_ops.insert(ID($and));
		config.generic_cbin_ops.insert(ID($or));
		config.generic_cbin_ops.insert(ID($xor));
		config.generic_cbin_ops.insert(ID($xnor));

		config.generic_bin_ops.insert(ID($shl));
		config.generic_bin_ops.insert(ID($shr));
		config.generic_bin_ops.insert(ID($sshl));
		config.generic_bin_ops.insert(ID($sshr));

		config.generic_bin_ops.insert(ID($lt));
		config.generic_bin_ops.insert(ID($le));
		config.generic_bin_ops.insert(ID($eq));
		config.generic_bin_ops.insert(ID($ne));
		config.generic_bin_ops.insert(ID($eqx));
		config.generic_bin_ops.insert(ID($nex));
		config.generic_bin_ops.insert(ID($ge));
		config.generic_bin_ops.insert(ID($gt));

		config.generic_cbin_ops.insert(ID($add));
		config.generic_cbin_ops.insert(ID($mul));

		config.generic_bin_ops.insert(ID($sub));
		config.generic_bin_ops.insert(ID($div));
		config.generic_bin_ops.insert(ID($mod));
		config.generic_bin_ops.insert(ID($divfloor));
		config.generic_bin_ops.insert(ID($modfloor));
		// config.generic_bin_ops.insert(ID($pow));

		config.generic_uni_ops.insert(ID($logic_not));
		config.generic_cbin_ops.insert(ID($logic_and));
		config.generic_cbin_ops.insert(ID($logic_or));

		config.generic_other_ops.insert(ID($alu));
		config.generic_other_ops.insert(ID($macc));

		log_header(design, "Executing SHARE pass (SAT-based resource sharing).\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-force") {
				config.opt_force = true;
				continue;
			}
			if (args[argidx] == "-aggressive") {
				config.opt_aggressive = true;
				continue;
			}
			if (args[argidx] == "-fast") {
				config.opt_fast = true;
				continue;
			}
			if (args[argidx] == "-limit" && argidx + 1 < args.size()) {
				config.limit = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-pattern-limit" && argidx + 1 < args.size()) {
				config.pattern_limit = atoi(args[++argidx].c_str());
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		ShareWorker sw(config, design);

		for (auto module : design->selected_modules())
			sw(module);
	}
} SharePass;

PRIVATE_NAMESPACE_END
