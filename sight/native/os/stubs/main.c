/*
 *  SIGHT - System information gathering hybrid tool
 *
 *  Copyright(c) 2007 Red Hat Middleware, LLC,
 *  and individual contributors as indicated by the @authors tag.
 *  See the copyright.txt in the distribution for a
 *  full listing of individual contributors.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library in the file COPYING.LIB;
 *  if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 *
 * @author Mladen Turk
 *
 */

#include "sight.h"
#include "sight_local.h"
#include "sight_types.h"
#include "sight_private.h"


apr_status_t sight_main(apr_pool_t *pool)
{
    return APR_SUCCESS;
}

SIGHT_EXPORT_DECLARE(void, Syslog, init0)(SIGHT_STDARGS,
                                          jstring domain)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(domain);
}

SIGHT_EXPORT_DECLARE(void, Syslog, close0)(SIGHT_STDARGS)
{

    UNREFERENCED_STDARGS;
}

SIGHT_EXPORT_DECLARE(void, Syslog, log0)(SIGHT_STDARGS,
                                         jint level,
                                         jstring msg)
{
    UNREFERENCED_STDARGS;
    UNREFERENCED(level);
    UNREFERENCED(msg);
}
