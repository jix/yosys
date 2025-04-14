/*
 *  ezSAT -- A simple and easy to use CNF generator for SAT solvers
 *
 *  Copyright (C) 2013  Claire Xenia Wolf <claire@yosyshq.com>
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

#include "ezcadical.h"

namespace CaDiCaL {

}

ezCadical::ezCadical() : cadicalSolver(NULL)
{
	foundContradiction = false;
}

ezCadical::~ezCadical()
{
	if (cadicalSolver != NULL)
		delete cadicalSolver;
}

void ezCadical::clear()
{
	if (cadicalSolver != NULL) {
		delete cadicalSolver;
		cadicalSolver = NULL;
	}
	foundContradiction = false;
	ezSAT::clear();
}

bool ezCadical::solver(const std::vector<int> &modelExpressions, std::vector<bool> &modelValues, const std::vector<int> &assumptions, std::vector<int> &failed)
{
	preSolverCallback();
	failed.clear();

	solverTimoutStatus = false;

	if (foundContradiction) {
		consumeCnf();
		return false;
	}

	std::vector<int> extraClauses, modelIdx;

	for (auto id : assumptions)
		extraClauses.push_back(bind(id));
	for (auto id : modelExpressions)
		modelIdx.push_back(bind(id));

	if (cadicalSolver == NULL) {
		cadicalSolver = new CaDiCaL::Solver;
        // TODO solver setup
	}

 	std::vector<std::vector<int>> cnf;
 	consumeCnf(cnf);

	if (cadicalSolver->vars() < numCnfVariables()) {
		cadicalSolver->reserve(numCnfVariables());
	}

	for (auto &clause : cnf) {
		for (auto idx : clause)
			cadicalSolver->add(idx);
        cadicalSolver->add(0);
	}

	for (auto idx : extraClauses)
		cadicalSolver->assume(idx);

	int status = cadicalSolver->solve();

    if (status != 10) {
		for (size_t i = 0; i < assumptions.size(); i++) {
			if (cadicalSolver->failed(extraClauses[i]))
				failed.push_back(assumptions[i]);
		}
        return false;
    }

	modelValues.clear();
	modelValues.resize(modelIdx.size());

	for (size_t i = 0; i < modelIdx.size(); i++)
	{
		int idx = modelIdx[i];
		bool refvalue = true;

		if (idx < 0)
			idx = -idx, refvalue = false;

        int value = cadicalSolver->val(idx) == idx;
		modelValues[i] = (value == refvalue);
	}

	return true;
}

