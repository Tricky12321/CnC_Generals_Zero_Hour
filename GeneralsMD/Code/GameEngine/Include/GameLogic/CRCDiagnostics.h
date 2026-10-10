/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Resync fork: CRC mismatch diagnostics.
//
// While the logic CRC is calculated, the running CRC is recorded after every object and every
// CRC section for the last few CRC frames. When the CRCs of the players differ, a report with
// these values is written to <user data>/CRCMismatch/. Comparing the reports of two players shows
// the first object or section where their game states differ.
//
// Changes of the FPU control word during the logic update are detected, logged to
// <user data>/CRCMismatch/FPUChanges.txt and repaired, because they make the floating point
// math of one machine differ from the others.

#pragma once

#include <stdio.h>
#include "Common/GameCommon.h"
#include "Common/GameType.h"
#include "Common/STLTypedefs.h"

class Object;

enum CRCDiagnosticsSection CPP_11(: Int)
{
	CRCSECTION_OBJECTS,
	CRCSECTION_RANDOM_SEED,
	CRCSECTION_PARTITION,
	CRCSECTION_PLAYERS,
	CRCSECTION_AI,
	CRCSECTION_COUNT
};

class CRCDiagnostics
{
public:
	CRCDiagnostics();

	void reset();																						///< Forget all recorded frames, call when a game starts or ends

	// Recording, called from GameLogic::getCRC
	void beginFrame(UnsignedInt frame);
	void recordObject(const Object *obj, UnsignedInt runningCRC);
	void recordSection(CRCDiagnosticsSection section, UnsignedInt runningCRC);
	void endFrame(UnsignedInt crc);

	/// Write a mismatch report. localCRC is the CRC this client sent, it identifies the recorded frame.
	/// Returns the file name of the report in the CRCMismatch directory, empty if none was written.
	AsciiString writeMismatchReport(UnsignedInt detectFrame, UnsignedInt localCRC, const std::map<Int, UnsignedInt> &playerCRCs,
		UnsignedInt consecutiveMismatches, UnsignedInt tolerance);

	// FPU monitoring
	void checkFPMode(const char *where, const Object *obj = nullptr);		///< Detect and repair a changed FPU control word
	void noteFPModeBeforeFrame();																			///< Detect a change between two logic frames, these are repaired by setFPMode()

private:
	struct ObjectRecord
	{
		ObjectID id;
		const char *templateName;
		UnsignedInt runningCRC;
		Coord3D position;
	};

	struct FrameRecord
	{
		UnsignedInt frame;
		UnsignedInt crc;
		UnsignedInt fpControl;
		Bool complete;
		UnsignedInt sectionCRC[CRCSECTION_COUNT];
		std::vector<ObjectRecord> objects;
	};

	enum { MAX_FRAMES = 4 };
	enum { MAX_FPU_LOG_LINES = 50 };

	FrameRecord *findFrameByCRC(UnsignedInt crc);
	FILE *openDiagnosticsFile(const char *fileName, const char *mode);

	FrameRecord m_frames[MAX_FRAMES];
	Int m_current;

	UnsignedInt m_fpuChangesInLogic;
	UnsignedInt m_fpuChangesBetweenFrames;
	UnsignedInt m_fpuLastChangeFrame;
	UnsignedInt m_fpuLastChangeValue;
	char m_fpuLastChangeWhere[160];
	UnsignedInt m_fpuLogLines;
	UnsignedInt m_reportsWritten;
};

/// Owned by GameLogic, so it is created and destroyed while the game memory manager is alive.
extern CRCDiagnostics *TheCRCDiagnostics;

/// The FPU control word that setFPMode() establishes.
UnsignedInt getExpectedFPControl();
