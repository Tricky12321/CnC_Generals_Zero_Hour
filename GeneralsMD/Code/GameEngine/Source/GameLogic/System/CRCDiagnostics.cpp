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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <float.h>

#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/CRCDiagnostics.h"
#include "GameLogic/FPUControl.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"

CRCDiagnostics *TheCRCDiagnostics = nullptr;

static const char *const s_sectionNames[CRCSECTION_COUNT] =
{
	"Objects",
	"RandomSeed",
	"PartitionManager",
	"PlayerList",
	"AI",
};

// ------------------------------------------------------------------------------------------------
CRCDiagnostics::CRCDiagnostics()
{
	reset();
}

// ------------------------------------------------------------------------------------------------
void CRCDiagnostics::reset()
{
	for (Int i = 0; i < MAX_FRAMES; ++i)
	{
		m_frames[i].frame = 0;
		m_frames[i].crc = 0;
		m_frames[i].fpControl = 0;
		m_frames[i].complete = FALSE;
		for (Int s = 0; s < CRCSECTION_COUNT; ++s)
		{
			m_frames[i].sectionCRC[s] = 0;
		}
		m_frames[i].objects.clear();
	}
	m_current = -1;

	m_fpuChangesInLogic = 0;
	m_fpuChangesBetweenFrames = 0;
	m_fpuLastChangeFrame = 0;
	m_fpuLastChangeValue = 0;
	m_fpuLastChangeWhere[0] = 0;
	m_fpuLogLines = 0;
	m_reportsWritten = 0;
}

// ------------------------------------------------------------------------------------------------
void CRCDiagnostics::beginFrame(UnsignedInt frame)
{
	m_current = (m_current + 1) % MAX_FRAMES;

	FrameRecord &rec = m_frames[m_current];
	rec.frame = frame;
	rec.crc = 0;
	rec.fpControl = _controlfp(0, 0);
	rec.complete = FALSE;
	for (Int s = 0; s < CRCSECTION_COUNT; ++s)
	{
		rec.sectionCRC[s] = 0;
	}
	rec.objects.clear();
}

// ------------------------------------------------------------------------------------------------
void CRCDiagnostics::recordObject(const Object *obj, UnsignedInt runningCRC)
{
	if (m_current < 0)
	{
		return;
	}

	ObjectRecord rec;
	rec.id = obj->getID();
	rec.templateName = obj->getTemplate() ? obj->getTemplate()->getName().str() : "<none>";
	rec.runningCRC = runningCRC;
	rec.position = *obj->getPosition();
	m_frames[m_current].objects.push_back(rec);
}

// ------------------------------------------------------------------------------------------------
void CRCDiagnostics::recordSection(CRCDiagnosticsSection section, UnsignedInt runningCRC)
{
	if (m_current < 0)
	{
		return;
	}

	m_frames[m_current].sectionCRC[section] = runningCRC;
}

// ------------------------------------------------------------------------------------------------
void CRCDiagnostics::endFrame(UnsignedInt crc)
{
	if (m_current < 0)
	{
		return;
	}

	m_frames[m_current].crc = crc;
	m_frames[m_current].complete = TRUE;
}

// ------------------------------------------------------------------------------------------------
CRCDiagnostics::FrameRecord *CRCDiagnostics::findFrameByCRC(UnsignedInt crc)
{
	// Search newest first, the same CRC value can appear twice if nothing changed in between.
	for (Int n = 0; n < MAX_FRAMES; ++n)
	{
		Int i = (m_current - n + MAX_FRAMES) % MAX_FRAMES;
		if (m_current >= 0 && m_frames[i].complete && m_frames[i].crc == crc)
		{
			return &m_frames[i];
		}
	}
	return nullptr;
}

// ------------------------------------------------------------------------------------------------
FILE *CRCDiagnostics::openDiagnosticsFile(const char *fileName, const char *mode)
{
	AsciiString dir = TheGlobalData->getPath_UserData();
	dir.concat("CRCMismatch");
	CreateDirectory(dir.str(), nullptr);

	AsciiString path;
	path.format("%s\\%s", dir.str(), fileName);
	return fopen(path.str(), mode);
}

// ------------------------------------------------------------------------------------------------
AsciiString CRCDiagnostics::writeMismatchReport(UnsignedInt detectFrame, UnsignedInt localCRC, const std::map<Int, UnsignedInt> &playerCRCs,
	UnsignedInt consecutiveMismatches, UnsignedInt tolerance)
{
	// One report per mismatch is enough, but a broken game must not fill the disk.
	static const UnsignedInt MAX_REPORTS_PER_GAME = 20;
	if (m_reportsWritten >= MAX_REPORTS_PER_GAME)
	{
		return AsciiString::TheEmptyString;
	}
	++m_reportsWritten;

	SYSTEMTIME now;
	GetLocalTime(&now);

	AsciiString fileName;
	fileName.format("CRCMismatch_%04d-%02d-%02d_%02d-%02d-%02d_frame%u.txt",
		now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, detectFrame);

	FILE *fp = openDiagnosticsFile(fileName.str(), "w");
	if (fp == nullptr)
	{
		DEBUG_LOG(("CRCDiagnostics - could not write %s", fileName.str()));
		return AsciiString::TheEmptyString;
	}

	FrameRecord *rec = findFrameByCRC(localCRC);
	if (rec == nullptr && TheGlobalData->m_resyncTestFrame != 0)
	{
		// -resyncTest sends a modified CRC, the recorded frame has the real one.
		rec = findFrameByCRC(localCRC ^ 0x5A5A5A5A);
	}

	fprintf(fp, "Zero Hour CRC mismatch report (resync fork)\n");
	fprintf(fp, "Time: %02d-%02d-%04d %02d:%02d:%02d\n", now.wDay, now.wMonth, now.wYear, now.wHour, now.wMinute, now.wSecond);
	fprintf(fp, "Map: %s\n", TheGlobalData->m_mapName.str());
	fprintf(fp, "Detected on frame: %u (mismatch %u of %u in a row)\n", detectFrame, consecutiveMismatches, tolerance);
	fprintf(fp, "CRC frame: %u\n", rec ? rec->frame : 0);

	Player *localPlayer = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	fprintf(fp, "Local player index: %d\n", localPlayer ? localPlayer->getPlayerIndex() : -1);
	fprintf(fp, "\nCRCs:\n");
	for (std::map<Int, UnsignedInt>::const_iterator it = playerCRCs.begin(); it != playerCRCs.end(); ++it)
	{
		Player *player = ThePlayerList ? ThePlayerList->getNthPlayer(it->first) : nullptr;
		AsciiString name;
		if (player)
		{
			name.translate(player->getPlayerDisplayName());
		}
		fprintf(fp, "  player %d slot %d %-20s 0x%8.8X%s\n", it->first,
			ThePlayerList ? ThePlayerList->getSlotIndex(it->first) : -1, name.str(), it->second,
			(player && player->isLocalPlayer()) ? "  (local)" : "");
	}

	fprintf(fp, "\nFPU:\n");
	fprintf(fp, "  expected control word: 0x%8.8X\n", getExpectedFPControl());
	fprintf(fp, "  control word at CRC:   0x%8.8X\n", rec ? rec->fpControl : 0);
	fprintf(fp, "  changes during logic update: %u\n", m_fpuChangesInLogic);
	fprintf(fp, "  changes between logic frames: %u\n", m_fpuChangesBetweenFrames);
	if (m_fpuChangesInLogic > 0)
	{
		fprintf(fp, "  last change: frame %u, value 0x%8.8X, after %s\n",
			m_fpuLastChangeFrame, m_fpuLastChangeValue, m_fpuLastChangeWhere);
	}

	if (rec == nullptr)
	{
		fprintf(fp, "\nNo recorded CRC frame matches the local CRC 0x%8.8X.\n", localCRC);
		fclose(fp);
		return AsciiString::TheEmptyString;
	}

	fprintf(fp, "\nSections (running CRC after the section):\n");
	for (Int s = 0; s < CRCSECTION_COUNT; ++s)
	{
		fprintf(fp, "  %-18s 0x%8.8X\n", s_sectionNames[s], rec->sectionCRC[s]);
	}

	fprintf(fp, "\nObjects (%u, running CRC after the object):\n", (UnsignedInt)rec->objects.size());
	fprintf(fp, "  index id       running_crc template                           x          y          z\n");
	for (size_t i = 0; i < rec->objects.size(); ++i)
	{
		const ObjectRecord &o = rec->objects[i];
		fprintf(fp, "  %5u %8u 0x%8.8X  %-32s %10.3f %10.3f %10.3f\n", (UnsignedInt)i, (UnsignedInt)o.id, o.runningCRC,
			o.templateName, o.position.x, o.position.y, o.position.z);
	}

	fclose(fp);
	DEBUG_LOG(("CRCDiagnostics - wrote %s", fileName.str()));
	return fileName;
}

// ------------------------------------------------------------------------------------------------
void CRCDiagnostics::checkFPMode(const char *where, const Object *obj)
{
	const UnsignedInt current = _controlfp(0, 0);
	const UnsignedInt expected = getExpectedFPControl();
	if (current == expected)
	{
		return;
	}

	++m_fpuChangesInLogic;
	m_fpuLastChangeFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;
	m_fpuLastChangeValue = current;
	if (obj && obj->getTemplate())
	{
		_snprintf(m_fpuLastChangeWhere, sizeof(m_fpuLastChangeWhere), "%s (object %u %s)", where, (UnsignedInt)obj->getID(), obj->getTemplate()->getName().str());
	}
	else
	{
		_snprintf(m_fpuLastChangeWhere, sizeof(m_fpuLastChangeWhere), "%s", where);
	}
	m_fpuLastChangeWhere[sizeof(m_fpuLastChangeWhere) - 1] = 0;

	DEBUG_LOG(("CRCDiagnostics - FPU control word changed to 0x%8.8X (expected 0x%8.8X) on frame %u after %s",
		current, expected, m_fpuLastChangeFrame, m_fpuLastChangeWhere));

	if (m_fpuLogLines < MAX_FPU_LOG_LINES)
	{
		++m_fpuLogLines;
		FILE *fp = openDiagnosticsFile("FPUChanges.txt", "a");
		if (fp)
		{
			SYSTEMTIME now;
			GetLocalTime(&now);
			fprintf(fp, "%02d-%02d-%04d %02d:%02d:%02d frame %u: control word 0x%8.8X, expected 0x%8.8X, after %s\n",
				now.wDay, now.wMonth, now.wYear, now.wHour, now.wMinute, now.wSecond,
				m_fpuLastChangeFrame, current, expected, m_fpuLastChangeWhere);
			fclose(fp);
		}
	}

	// Repair it, so the rest of the frame is calculated like on the other machines.
	setFPMode();
}

// ------------------------------------------------------------------------------------------------
void CRCDiagnostics::noteFPModeBeforeFrame()
{
	if (_controlfp(0, 0) != getExpectedFPControl())
	{
		++m_fpuChangesBetweenFrames;
	}
}
