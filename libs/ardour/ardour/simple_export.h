/*
 * Copyright (C) 2022 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _ardour_simple_export_h_
#define _ardour_simple_export_h_

#include "ardour/libardour_visibility.h"
#include "ardour/session_handle.h"
#include "ardour/types.h"

namespace ARDOUR
{
class ExportHandler;
class ExportStatus;
class ExportProfileManager;

/** Base class for audio export
 *
 * This allows one to export audio from the session's
 * master bus using a given export-preset.
 */
class LIBARDOUR_API SimpleExport : public ARDOUR::SessionHandlePtr
{
public:
	SimpleExport ();
	virtual ~SimpleExport () {}

	void set_session (ARDOUR::Session*);

	/** Do everything up to starting the render, and return while it runs.
	 *
	 * run_export() below waits for the render and finishes it, which is what a
	 * command line tool wants and what a UI thread must never do: the wait is
	 * as long as the export.  A host with a thread of its own calls this, polls
	 * ExportStatus, and calls finish_export() when the render stops.
	 */
	bool start_export ();
	/** Leaves freewheel and reports whether the render was aborted.  Must be
	 * called on the thread that drives the session. */
	bool finish_export ();

	bool run_export ();

	void set_name (std::string const&);
	void set_folder (std::string const&);
	void set_range (samplepos_t, samplepos_t);
	bool set_preset (std::string const&);

	std::string preset_uuid () const;
	std::string folder () const;
	bool        check_outputs () const;

protected:
	std::shared_ptr<ARDOUR::ExportHandler>        _handler;
	std::shared_ptr<ARDOUR::ExportStatus>         _status;
	std::shared_ptr<ARDOUR::ExportProfileManager> _manager;

private:
	std::string _name;
	std::string _folder;
	std::string _pset_id;
	samplepos_t _start;
	samplepos_t _end;
};

}

#endif
