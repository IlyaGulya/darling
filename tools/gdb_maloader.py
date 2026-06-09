# Copyright 2011 Shinichiro Hamaji. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY Shinichiro Hamaji ``AS IS'' AND ANY EXPRESS OR
# IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
# CONTRIBUTORS BE LIABLE FOR ANY DAMAGES ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import gdb
import os
import re
import sys


def bt(demangle=True):
    frame = gdb.selected_frame()
    while True:
        newer = frame.newer()
        if not newer:
            break
        frame = newer

    pipe = os.popen("c++filt", "w") if demangle else sys.stdout
    index = 0
    while frame:
        output = gdb.execute("p dumpSymbol((void*)0x%x)" % frame.pc(), to_string=True)
        match = re.match(r'.*"(.*)"$', output)
        if match:
            pipe.write("#%-2d %s\n" % (index, match.group(1)))
        else:
            sal = frame.find_sal()
            location = ""
            if sal.symtab:
                location = "at %s:%d" % (sal.symtab, sal.line)
            else:
                soname = gdb.solib_name(frame.pc())
                if soname:
                    location = "from %s" % soname
            name = frame.name() or "??"
            pipe.write("#%-2d 0x%016x in %s () %s\n" % (index, frame.pc(), name, location))
        frame = frame.older()
        index += 1

    pipe.close()
