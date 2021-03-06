﻿/**
  * Touhou Community Reliant Automatic Patcher
  * Tasogare Frontier support plugin
  *
  * ----
  *
  * Tasofro plugins patching.
  */

#include "thcrap.h"
#include "thcrap_tasofro.h"
#include "plugin.h"
#include <map>

static json_t *cur_patch = nullptr;

int BP_detour_plugin(x86_reg_t *regs, json_t *bp_info)
{
	// Parameters
	// ----------
	HMODULE *plugin = (HMODULE*)json_object_get_register(bp_info, regs, "plugin");
	// ----------

	iat_detour_apply(*plugin);

	if (cur_patch) {
		json_t *binhacks = json_object_get(cur_patch, "binhacks");
		if (binhacks) {
			binhacks_apply(binhacks, *plugin);
		}
		json_decref(cur_patch);
		cur_patch = nullptr;
	}

	return 1;
}

int patch_dll(void*, size_t, size_t, const char*, json_t *patch)
{
	// We only want to make thcrap load the jdiff file, we'll patch the DLL file after its sections are mapped in memory.
	if (cur_patch) {
		json_decref(cur_patch);
	}
	cur_patch = patch;
	json_incref(cur_patch);
	return 0;
}

LPCSTR WINAPI tasofro_CharNextA(LPSTR lpsz)
{
	return CharNextU(lpsz);
}
