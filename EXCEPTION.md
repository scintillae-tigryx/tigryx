# Tigryx VM — GPL Linking & Runtime Exception

Version 1.0

This file describes an additional permission ("the Exception") that applies to
the Tigryx virtual machine and runtime ("the Program") in addition to, and on
top of, the GNU General Public License, version 3 ("the GPL"), under which the
Program is licensed.

Copyright (c) 2026 Scintillae Ltd. All rights reserved.

The Program is free software: you may redistribute it and/or modify it under
the terms of the GPL as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version. The Exception
below grants you additional permissions; it does not restrict any rights you
already have under the GPL.

If you do not wish to be bound by the conditions of the GPL, or if your
intended use is not permitted by the Exception below, a separate commercial
license is available from the copyright holder. See COMMERCIAL.md.

--------------------------------------------------------------------------------

## 1. Definitions

"The Program" means the Tigryx virtual machine, interpreter, runtime libraries,
and any other files distributed under the GPL together with this Exception. It
does NOT include the Tigryx standard library artifacts (for example, `info.tgx`,
`strings.tgx`, and the other library `.tgx` sources/modules), which are
distributed under the separate MIT license so that they may be used equally
freely in both the GPL and the commercial editions of the product.

"Independent Module" means a unit of program logic — including, but not limited
to, a Tigryx bytecode file (`.tlx`), source program (`.tgx`), data file, asset,
or plug-in — that is authored separately from the Program and that interacts
with the Program only through the Program's documented public interfaces (its
loading, execution, host, and FFI interfaces). An Independent Module is not, by
virtue of being loaded, interpreted, or executed by the Program, a work derived
from the Program.

"Aggregate Distribution" means distributing an unmodified, separately
identifiable copy of the Program (for example, the `tigryx` runtime executable
or a runtime shared/static library) alongside one or more Independent Modules,
where the Program and the Independent Modules remain distinct files on the
distribution medium.

"Combined Executable" means a single executable file, image, or archive in
which the Program (in whole or in part, whether as object code, bytecode, or
otherwise) and one or more Independent Modules are merged, embedded, packed,
linked, or otherwise combined such that they are no longer separately
identifiable files. The packed-executable output of the commercial toolchain
is a Combined Executable.

## 2. Grant of Additional Permission

As an exception to the terms of the GPL, you have permission to:

(a) **Run** any Independent Module on, with, or under the Program, for any
    purpose whatsoever, commercial or non-commercial, without your Independent
    Module thereby becoming subject to the GPL; and

(b) **Aggregate Distribution** — distribute an unmodified copy of the Program
    together with your Independent Modules — and license, sell, or otherwise
    convey your Independent Modules under terms of your own choosing,

provided that, in each case, you continue to satisfy the conditions of the GPL
with respect to the Program itself (Section 4).

The mere act of an Independent Module using the Program's public interfaces,
being loaded by the Program, or being interpreted or executed by the Program
does not cause that Independent Module to be a derivative work of the Program,
and does not place that Independent Module under the GPL.

## 3. Scope and Limits of the Exception

This Exception does **NOT** extend to, and expressly excludes, the creation or
distribution of a **Combined Executable**. Producing a Combined Executable
(including via the commercial toolchain's packed-executable mode) creates a single combined
work that, absent this Exception, is governed in its entirety by the GPL.
Accordingly:

(a) You may create and distribute a Combined Executable only under the terms of
    the GPL — meaning you must release the complete corresponding source of the
    Combined Executable, including your Independent Modules, under the GPL; OR

(b) You may obtain a separate commercial license from the copyright holder that
    permits you to create and distribute Combined Executables under terms of
    your own choosing. See COMMERCIAL.md.

This Exception also does not grant any permission with respect to the
commercial-only tools and platforms (for example, the native compilers and
the bound-executable runtime), which are not part of the
Program and are licensed separately.

## 4. Conditions

This Exception applies only so long as you comply with the GPL with respect to
the Program. In particular, if you modify the Program itself, the modified
Program remains subject to the GPL and must be released as required by the GPL.
The Exception covers your Independent Modules; it never relieves you of your
obligations regarding the Program's own source.

If you convey a copy of the Program (modified or unmodified), you must keep this
Exception notice intact, or — if you choose to remove the Exception — convey the
Program under the plain GPL without the additional permission.

## 5. No Other Rights

Except for the additional permission expressly granted above, all terms of the
GPL apply unchanged. Nothing in this Exception constitutes a waiver of any other
right or remedy of the copyright holder. This Exception is granted by the
copyright holder and may be relied upon by any recipient of the Program.

--------------------------------------------------------------------------------

SUMMARY (non-binding, for convenience only):

  - Write a game/app as `.tgx` / `.tlx`?                         FREE
  - Run it on the Tigryx VM, even commercially?                  FREE
  - Ship the unmodified VM next to your `.tlx` files?           FREE (aggregate)
  - Modify the VM itself and distribute it?                     GPL (publish source)
  - Pack everything into one `.exe`?                           NEEDS COMMERCIAL LICENSE
                                                                (or release under GPL)

The binding terms are in Sections 1–5 and in the GPL itself. Where this summary
and the binding terms conflict, the binding terms control.
