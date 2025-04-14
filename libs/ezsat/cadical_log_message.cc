/*

This file is a modified version of cadical's message.cpp and terminal.cpp

MIT License

Copyright (c) 2016-2021 Armin Biere, Johannes Kepler University Linz, Austria
Copyright (c) 2020-2021 Mathias Fleury, Johannes Kepler University Linz, Austria
Copyright (c) 2020-2021 Nils Froleyks, Johannes Kepler University Linz, Austria
Copyright (c) 2022-2024 Katalin Fazekas, Vienna University of Technology, Austria
Copyright (c) 2021-2024 Armin Biere, University of Freiburg, Germany
Copyright (c) 2021-2024 Mathias Fleury, University of Freiburg, Germany
Copyright (c) 2023-2024 Florian Pollitt, University of Freiburg, Germany
Copyright (c) 2024-2024 Tobias Faller, University of Freiburg, Germany

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

 */

#include "libs/cadical/src/internal.hpp"

#include "kernel/log.h"

USING_YOSYS_NAMESPACE

namespace CaDiCaL
{

Terminal::Terminal(FILE *f) : file(f), reset_on_exit(false) { use_colors = connected = false; }

void Terminal::force_colors() {}
void Terminal::force_no_colors() {}
void Terminal::force_reset_on_exit() {}

void Terminal::reset() {}

void Terminal::disable() {}

Terminal::~Terminal() { (void)reset_on_exit; }

Terminal tout(stdout);
Terminal terr(stderr);

/*------------------------------------------------------------------------*/
#ifndef QUIET
/*------------------------------------------------------------------------*/

void Internal::print_prefix() { log("[cadical] %s", prefix.c_str()); }

void Internal::vmessage(const char *fmt, va_list &ap)
{
#ifdef LOGGING
	if (!opts.log)
#endif
		if (opts.quiet)
			return;
	print_prefix();

	logv(fmt, ap);
	log("\n");
}

void Internal::message(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vmessage(fmt, ap);
	va_end(ap);
}

void Internal::message()
{
#ifdef LOGGING
	if (!opts.log)
#endif
		if (opts.quiet)
			return;
	print_prefix();
	log("\n");
}

/*------------------------------------------------------------------------*/

void Internal::vverbose(int level, const char *fmt, va_list &ap)
{
#ifdef LOGGING
	if (!opts.log)
#endif
		if (opts.quiet || level > opts.verbose)
			return;
	print_prefix();
	logv(fmt, ap);
	log("\n");
}

void Internal::verbose(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vverbose(level, fmt, ap);
	va_end(ap);
}

void Internal::verbose(int level)
{
#ifdef LOGGING
	if (!opts.log)
#endif
		if (opts.quiet || level > opts.verbose)
			return;
	print_prefix();
	log("\n");
}

/*------------------------------------------------------------------------*/

void Internal::section(const char *title)
{
#ifdef LOGGING
	if (!opts.log)
#endif
		if (opts.quiet)
			return;
	if (stats.sections++)
		MSG();
	print_prefix();
	//   tout.blue ();
	log("%s\n", "--- [ ");
	//   tout.blue (true);
	log("%s\n", title);
	//   tout.blue ();
	log("%s\n", " ] ");
	for (int i = strlen(title) + strlen(prefix.c_str()) + 9; i < 78; i++)
		log("%c", '-');
	//   tout.normal ();
	log("\n");
	MSG();
}

/*------------------------------------------------------------------------*/

void Internal::phase(const char *phase, const char *fmt, ...)
{
#ifdef LOGGING
	if (!opts.log)
#endif
		if (opts.quiet || (!force_phase_messages && opts.verbose < 2))
			return;
	print_prefix();
	log("[%s] ", phase);
	va_list ap;
	va_start(ap, fmt);
	logv(fmt, ap);
	va_end(ap);
	log("\n");
}

void Internal::phase(const char *phase, int64_t count, const char *fmt, ...)
{
#ifdef LOGGING
	if (!opts.log)
#endif
		if (opts.quiet || (!force_phase_messages && opts.verbose < 2))
			return;
	print_prefix();
	log("[%s-%" PRId64 "] ", phase, count);
	va_list ap;
	va_start(ap, fmt);
	logv(fmt, ap);
	va_end(ap);
	log("\n");
}

/*------------------------------------------------------------------------*/
#endif // ifndef QUIET
/*------------------------------------------------------------------------*/

void Internal::warning(const char *fmt, ...)
{
	//   terr.bold ();
	log_warning("cadical: ");
	//   terr.red (1);
	log_warning_noprefix("warning:");
	//   terr.normal ();
	log_warning_noprefix(" ");
	va_list ap;
	va_start(ap, fmt);
	logv_warning_noprefix(fmt, ap);
	va_end(ap);
	log_warning_noprefix("\n");
}

/*------------------------------------------------------------------------*/

void Internal::error_message_start()
{
	//   terr.bold ();
	log_warning("cadical: ");
	//   terr.red (1);
	log_warning_noprefix("error:");
	//   terr.normal ();
	log_warning_noprefix(" ");
}

void Internal::error_message_end()
{
	log_warning_noprefix("\n");
	log_error("cadical error\n");
}

void Internal::verror(const char *fmt, va_list &ap)
{
	error_message_start();
	logv_warning_noprefix(fmt, ap);
	error_message_end();
}

void Internal::error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	verror(fmt, ap);
	va_end(ap); // unreachable
}

/*------------------------------------------------------------------------*/

void fatal_message_start()
{
	//   terr.bold ();
	log_warning("cadical: ");
	//   terr.red (1);
	log_warning_noprefix("fatal error:");
	//   terr.normal ();
	log_warning_noprefix(" ");
}

void fatal_message_end()
{
	log_warning_noprefix("\n");
	log_error("cadical fatal error\n");
}

void fatal(const char *fmt, ...)
{
	fatal_message_start();
	va_list ap;
	va_start(ap, fmt);
	logv_warning_noprefix(fmt, ap);
	va_end(ap);
	fatal_message_end();
	log_error("cadical fatal error\n");
}

} // namespace CaDiCaL
