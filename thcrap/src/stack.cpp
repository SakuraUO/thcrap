/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Patch stack evaluators and information.
  */

#include "thcrap.h"
#include "vfs.h"

static json_t* resolve_chain_default(const char *fn)
{
	json_t *ret = fn ? json_array() : NULL;
	char *fn_build = fn_for_build(fn);
	json_array_append_new(ret, json_string(fn));
	json_array_append_new(ret, json_string(fn_build));
	SAFE_FREE(fn_build);
	return ret;
}

static json_t* resolve_chain_game_default(const char *fn)
{
	char *fn_common = fn_for_game(fn);
	const char *fn_common_ptr = fn_common ? fn_common : fn;
	json_t *ret = resolve_chain(fn_common_ptr);
	SAFE_FREE(fn_common);
	return ret;
}

static resolve_chain_t resolve_chain_function      = resolve_chain_default;
static resolve_chain_t resolve_chain_game_function = resolve_chain_game_default;

json_t* resolve_chain(const char *fn)
{
	return resolve_chain_function(fn);
}

void set_resolve_chain(resolve_chain_t function)
{
	resolve_chain_function = function;
}

json_t* resolve_chain_game(const char *fn)
{
	return resolve_chain_game_function(fn);
}

void set_resolve_chain_game(resolve_chain_t function)
{
	resolve_chain_game_function = function;
}

int stack_chain_iterate(stack_chain_iterate_t *sci, const json_t *chain, sci_dir_t direction)
{
	int ret = 0;
	size_t chain_size = json_array_size(chain);
	if(sci && direction && chain_size) {
		int chain_idx;
		// Setup
		if(!sci->patches) {
			sci->patches = json_object_get(run_cfg, "patches");
			sci->step =
				(direction < 0) ? (json_array_size(sci->patches) * chain_size) - 1 : 0
			;
		} else {
			sci->step += direction;
		}
		chain_idx = sci->step % chain_size;
		sci->fn = json_array_get_string(chain, chain_idx);
		if(chain_idx == (direction < 0) * (chain_size - 1)) {
			sci->patch_info = json_array_get(sci->patches, sci->step / chain_size);
		}
		ret = sci->patch_info != NULL;
	}
	return ret;
}

json_t* stack_json_resolve_chain(const json_t *chain, size_t *file_size)
{
	json_t *ret = NULL;
	stack_chain_iterate_t sci = {0};
	size_t json_size = 0;

	json_t *obj;
	size_t n;
	json_array_foreach(chain, n, obj) {
		const char *fn = json_string_value(obj);
		size_t size = 0;
		json_t *json_new = jsonvfs_get(fn, &size);
		if (json_new) {
			if (!ret) {
				ret = json_new;
			}
			else {
				json_object_merge(ret, json_new);
				json_decref(json_new);
			}
			log_printf("\n+ vfs:%s", fn);
			json_size += size;
		}
	}

	while(stack_chain_iterate(&sci, chain, SCI_FORWARDS)) {
		json_size += patch_json_merge(&ret, sci.patch_info, sci.fn);
	}
	log_printf(ret ? "\n" : "not found\n");
	if(file_size) {
		*file_size = json_size;
	}
	return ret;
}

json_t* stack_json_resolve(const char *fn, size_t *file_size)
{
	json_t *ret = NULL;
	json_t *chain = resolve_chain(fn);
	if(json_array_size(chain)) {
		log_printf("(JSON) Resolving %s... ", fn);
		ret = stack_json_resolve_chain(chain, file_size);
	}
	json_decref(chain);
	return ret;
}

void* stack_file_resolve_chain(const json_t *chain, size_t *file_size)
{
	void *ret = NULL;
	stack_chain_iterate_t sci = {0};

	// Empty stacks are a thing, too.
	if(file_size) {
		*file_size = 0;
	}

	// Both the patch stack and the chain have to be traversed backwards: Later
	// patches take priority over earlier ones, and build-specific files are
	// preferred over generic ones.
	while(stack_chain_iterate(&sci, chain, SCI_BACKWARDS) && !ret) {
		ret = patch_file_load(sci.patch_info, sci.fn, file_size);
		if(ret) {
			patch_print_fn(sci.patch_info, sci.fn);
		}
	}
	log_printf(ret ? "\n" : "not found\n");
	return ret;
}

char* stack_fn_resolve_chain(const json_t *chain)
{
	stack_chain_iterate_t sci = { 0 };

	// Both the patch stack and the chain have to be traversed backwards: Later
	// patches take priority over earlier ones, and build-specific files are
	// preferred over generic ones.
	while (stack_chain_iterate(&sci, chain, SCI_BACKWARDS)) {
		char *fn = fn_for_patch(sci.patch_info, sci.fn);
		DWORD attr = GetFileAttributesU(fn);
		if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
			return fn;
		}
		free(fn);
	}
	return nullptr;
}

void* stack_game_file_resolve(const char *fn, size_t *file_size)
{
	void *ret = NULL;
	json_t *chain = resolve_chain_game(fn);
	if(json_array_size(chain)) {
		log_printf("(Data) Resolving %s... ", json_array_get_string(chain, 0));
		ret = stack_file_resolve_chain(chain, file_size);
	}
	json_decref(chain);
	return ret;
}

json_t* stack_game_json_resolve(const char *fn, size_t *file_size)
{
	char *full_fn = fn_for_game(fn);
	json_t *ret = stack_json_resolve(full_fn, file_size);
	SAFE_FREE(full_fn);
	return ret;
}

void stack_show_missing(void)
{
	json_t *patches = json_object_get(run_cfg, "patches");
	size_t i;
	json_t *patch_info;
	json_t *rem_arcs = json_array();
	// Don't forget the null terminator...
	size_t rem_arcs_str_len = 1;

	json_array_foreach(patches, i, patch_info) {
		json_t *archive = json_object_get(patch_info, "archive");
		if(json_is_string(archive) && !PathFileExists(json_string_value(archive))) {
			json_array_append(rem_arcs, archive);
			rem_arcs_str_len += 1 + json_string_length(archive) + 1;
		}
	}

	if(json_array_size(rem_arcs) > 0) {
		VLA(char, rem_arcs_str, rem_arcs_str_len);
		size_t i;
		json_t *archive_obj;
		char *p = rem_arcs_str;

		json_array_foreach(rem_arcs, i, archive_obj) {
			if(json_is_string(archive_obj)) {
				p += sprintf(p, "\t%s\n", json_string_value(archive_obj));
			}
		}
		log_mboxf(NULL, MB_OK | MB_ICONEXCLAMATION,
			"Some patches in your configuration could not be found:\n"
			"\n"
			"%s"
			"\n"
			"Please reconfigure your patch stack - either by running the configuration tool, "
			"or by simply editing your run configuration file (%s).",
			rem_arcs_str, json_object_get_string(runconfig_get(), "run_cfg_fn")
		);
		VLA_FREE(rem_arcs_str);
	}
	json_decref(rem_arcs);
}

/// Customizable per-patch message on startup
/// -----------------------------------------
void patch_show_motd(json_t *patch_info)
{
	auto msg = json_object_get_string(patch_info, "motd");
	auto title = json_object_get_string(patch_info, "motd_title");
	auto type = json_object_get_hex(patch_info, "motd_type");
	if(!msg) {
		return;
	}
	if(!title) {
		const auto TITLE_FMT = "Message from %s";
		const auto patch_id = json_object_get_string(patch_info, "id");

		auto motd_title_size = _scprintf(TITLE_FMT, patch_id) + 1;
		VLA(char, title_auto, motd_title_size);
		sprintf(title_auto, TITLE_FMT, patch_id);

		log_mboxf(title_auto, type, msg);
		VLA_FREE(title_auto);
	} else {
		log_mboxf(title, type, msg);
	}
}

void stack_show_motds(void)
{
	json_t *patches = json_object_get(run_cfg, "patches");
	size_t i;
	json_t *patch_info;
	json_array_foreach(patches, i, patch_info) {
		patch_show_motd(patch_info);
	}
}

extern "C" __declspec(dllexport) void motd_mod_post_init(void)
{
	stack_show_motds();
}
/// -----------------------------------------

int stack_remove_if_unneeded(const char *patch_id)
{
	auto runconfig = runconfig_get();
	auto game = json_object_get(runconfig, "game");
	auto build = json_object_get(runconfig, "build");
	auto patches = json_object_get(runconfig, "patches");
	size_t i;
	json_t *patch_info;

	// (No early return if we have no game name, since we want
	//  to slice out the patch in that case too.)

	json_array_foreach(patches, i, patch_info) {
		const char *id = json_object_get_string(patch_info, "id");
		if(!id || strcmp(id, patch_id)) {
			continue;
		}
		int game_found = 0;
		if(json_is_string(game)) {
			auto game_len = json_string_length(game);
			auto game_val = json_string_value(game);
			auto build_len = json_string_length(build);
			auto build_val = json_string_value(build);

			size_t js_len = game_len + 1 + build_len + strlen(".js") + 1;
			VLA(char, js, js_len);

			// <game>.js
			sprintf(js, "%s.js", game_val);
			game_found |= patch_file_exists(patch_info, js);

			// <game>/ (directory)
			game_found |= patch_file_exists(patch_info, game_val);

			if(build_val && !game_found) {
				// <game>.<build>.js
				sprintf(js, "%s.%s.js", game_val, build_val);
				game_found |= patch_file_exists(patch_info, js);
			}
			VLA_FREE(js);
		}
		if(!game_found) {
			json_array_remove(patches, i);
		}
		return !game_found;
	}
	return -1;
}
