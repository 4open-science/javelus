/*
* Copyright (C) 2012  Tianxiao Gu. All rights reserved.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*
* Please contact Instituite of Computer Software, Nanjing University,
* 163 Xianlin Avenue, Nanjing, Jiangsu Provience, 210046, China,
* or visit moon.nju.edu.cn if you need additional information or have any
* questions.
*/


#include "precompiled.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/dsuFlags.hpp"
#ifdef TARGET_OS_FAMILY_linux
# include "os_linux.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_solaris
# include "os_solaris.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_windows
# include "os_windows.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_aix
# include "os_aix.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_bsd
# include "os_bsd.inline.hpp"
#endif
// -------------------- DSUFlags -----------------------------------------

void DSUFlags::atomic_set_bits(jint bits) {
  // Atomically update the flags with the bits given
  jint old_flags, new_flags, f;
  do {
    old_flags = _flags;
    new_flags = old_flags | bits;
    f = Atomic::cmpxchg(new_flags, &_flags, old_flags);
  } while(f != old_flags);
}

void DSUFlags::atomic_clear_bits(jint bits) {
  // Atomically update the flags with the bits given
  jint old_flags, new_flags, f;
  do {
    old_flags = _flags;
    new_flags = old_flags & ~bits;
    f = Atomic::cmpxchg(new_flags, &_flags, old_flags);
  } while(f != old_flags);
}



#if !defined(PRODUCT) || INCLUDE_JVMTI

void DSUFlags::print_on(outputStream* st) const {
  st->print("%d", _flags);
}

#endif // !PRODUCT || INCLUDE_JVMTI
