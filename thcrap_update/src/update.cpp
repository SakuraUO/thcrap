/**
  * Touhou Community Reliant Automatic Patcher
  * Update module
  *
  * ----
  *
  * Main updating functionality.
  */

#include <thcrap.h>
#include <unordered_map>
#include <wininet.h>
#include <zlib.h>
#include "update.h"

SRWLOCK inet_srwlock = {SRWLOCK_INIT};
double perffreq;

typedef struct {
	BYTE *file_buffer;
	DWORD file_size;

	// Absolute timestamps.
	LONGLONG time_start;
	LONGLONG time_ping;
	LONGLONG time_end;
} download_context_t;

HINTERNET http_init(void)
{
	HINTERNET ret;
	LARGE_INTEGER pf;
	DWORD ignore = 1;
	// DWORD timeout = 500;

	// Format according to RFC 7231, section 5.5.3
	const stringref_t AGENT_FORMAT("%s/%s (%s)");
	auto self_name = PROJECT_NAME_SHORT();
	auto self_version = PROJECT_VERSION_STRING();
	auto os = windows_version();

	size_t agent_len = 0;
	agent_len += AGENT_FORMAT.len;
	agent_len += strlen(self_name);
	agent_len += strlen(self_version);
	agent_len += strlen(os);

	VLA(char, agent, agent_len + 1);
	sprintf(agent, AGENT_FORMAT.str, self_name, self_version, os);

	QueryPerformanceFrequency(&pf);
	perffreq = (double)pf.QuadPart;

	ret = InternetOpenU(agent, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if(!ret) {
		// No internet access...?
		return nullptr;
	}
	/*
	InternetSetOption(ret, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(DWORD));
	InternetSetOption(ret, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(DWORD));
	InternetSetOption(ret, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(DWORD));
	*/

	// This is necessary when Internet Explorer is set to "work offline"... which
	// will essentially block all wininet HTTP accesses on handles that do not
	// explicitly ignore this setting.
	InternetSetOption(ret, INTERNET_OPTION_IGNORE_OFFLINE, &ignore, sizeof(DWORD));

	VLA_FREE(agent);
	return ret;
}

HINTERNET* http_handle(void)
{
	static HINTERNET hInternet = http_init();
	return &hInternet;
}

get_result_t http_get(download_context_t *ctx, const char *url, file_callback_t callback, void *callback_param)
{
	get_result_t get_ret = GET_INVALID_PARAMETER;
	DWORD byte_ret = sizeof(DWORD);
	DWORD http_stat = 0;
	DWORD file_size_local = 0;
	auto hHTTP = *http_handle();
	HINTERNET hFile = NULL;

	if(!hHTTP || !url) {
		return GET_INVALID_PARAMETER;
	}
	AcquireSRWLockShared(&inet_srwlock);

	ZeroMemory(ctx, sizeof(download_context_t));

	QueryPerformanceCounter((LARGE_INTEGER *)&ctx->time_start);
	hFile = InternetOpenUrl(
		hHTTP, url, NULL, 0,
		INTERNET_FLAG_RELOAD | INTERNET_FLAG_KEEP_CONNECTION, 0
	);
	QueryPerformanceCounter((LARGE_INTEGER *)&ctx->time_ping);
	if(!hFile) {
		// TODO: We should use FormatMessage() for both coverage and i18n
		// reasons here, but its messages are way too verbose for my taste.
		// So let's wait with that until this code is used in conjunction
		// with a GUI, if at all.
		DWORD inet_ret = GetLastError();
		switch(inet_ret) {
		case ERROR_INTERNET_NAME_NOT_RESOLVED:
			log_printf("Could not resolve hostname\n", inet_ret);
			break;
		case ERROR_INTERNET_CANNOT_CONNECT:
			log_printf("Connection refused\n", inet_ret);
			break;
		case ERROR_INTERNET_TIMEOUT:
			log_printf("timed out\n", inet_ret);
			break;
		case ERROR_INTERNET_UNRECOGNIZED_SCHEME:
			log_print("Unknown protocol\n");
			break;
		default:
			log_printf("WinInet error %d\n", inet_ret);
			break;
		}
		get_ret = GET_SERVER_ERROR;
		goto end;
	}

	HttpQueryInfo(hFile, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_STATUS_CODE,
		&http_stat, &byte_ret, 0
	);
	log_printf("%d ", http_stat);
	if(http_stat != 200) {
		get_ret = GET_NOT_AVAILABLE;
		goto end;
	}

	HttpQueryInfo(hFile, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_CONTENT_LENGTH,
		&ctx->file_size, &byte_ret, 0
	);
	ctx->file_buffer = (BYTE*)malloc(ctx->file_size);
	if(ctx->file_buffer) {
		BYTE *p = ctx->file_buffer;
		DWORD rem_size = ctx->file_size;
		if (callback) {
			callback(url, GET_OK, 0, ctx->file_size, callback_param);
		}
		while(rem_size) {
			DWORD read_size = 0;
			if(!InternetQueryDataAvailable(hFile, &read_size, 0, 0)) {
				read_size = rem_size;
			}
			if(read_size == 0) {
				log_printf("disconnected\n");
				get_ret = GET_SERVER_ERROR;
				goto end;
			}
			if(InternetReadFile(hFile, p, read_size, &byte_ret)) {
				rem_size -= byte_ret;
				p += byte_ret;
				if (callback) {
					if (callback(url, GET_OK, ctx->file_size - rem_size, ctx->file_size, callback_param) == FALSE) {
						get_ret = GET_CANCELLED;
						goto end;
					}
				}
			} else {
				SAFE_FREE(ctx->file_buffer);
				log_printf("\nReading error #%d! ", GetLastError());
				get_ret = GET_SERVER_ERROR;
				goto end;
			}
		}
		get_ret = GET_OK;
	} else {
		get_ret = GET_OUT_OF_MEMORY;
	}
end:
	QueryPerformanceCounter((LARGE_INTEGER *)&ctx->time_end);
	if (get_ret != GET_OK && callback) {
		callback(url, get_ret, 0, ctx->file_size, callback_param);
	}

	InternetCloseHandle(hFile);
	ReleaseSRWLockShared(&inet_srwlock);
	return get_ret;
}

void http_mod_exit(void)
{
	auto phHTTP = http_handle();
	if(*phHTTP) {
		AcquireSRWLockExclusive(&inet_srwlock);
		InternetCloseHandle(*phHTTP);
		*phHTTP = nullptr;
		ReleaseSRWLockExclusive(&inet_srwlock);
	}
}

/// Internal C++ server connection handling
/// ---------------------------------------
LONGLONG server_t::ping_average() const
{
	LONGLONG ret = 0;
	int divisor = 0;
	for(auto i : this->ping) {
		ret += i;
		if(i != 0) {
			divisor++;
		}
	}
	if(divisor == 0) {
		return 0;
	}
	return ret / divisor;
}

void server_t::ping_push(LONGLONG newval)
{
	auto type_size = sizeof(this->ping[0]);
	auto elements = sizeof(this->ping) / type_size;
	memmove(&this->ping[0], &this->ping[1], (elements - 1) * type_size);
	ping[elements - 1] = newval;
}

void* server_t::download(
	DWORD *file_size, get_result_t *ret, const char *fn, const DWORD *exp_crc, file_callback_t callback, void *callback_param
)
{
	assert(file_size);

	get_result_t temp_ret;
	download_context_t ctx;
	URL_COMPONENTSA uc = {0};
	auto server_len = strlen(this->url) + 1;
	// * 3 because characters may be URL-encoded
	DWORD url_len = server_len + (strlen(fn) * 3) + 1;
	VLA(char, server_host, server_len);
	VLA(char, url, url_len);

	if(!ret) {
		ret = &temp_ret;
	}

	InternetCombineUrl(this->url, fn, url, &url_len, 0);

	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = server_host;
	uc.dwHostNameLength = server_len;

	InternetCrackUrl(this->url, 0, 0, &uc);

	log_printf("%s (%s)... ", fn, uc.lpszHostName);

	*ret = http_get(&ctx, url, callback, callback_param);
	*file_size = ctx.file_size;

	VLA_FREE(url);
	VLA_FREE(server_host);

	auto fail = [this, &ctx] (const char *reason) {
		if(reason) {
			log_printf("%s\n", reason);
		}
		this->disable();
		SAFE_FREE(ctx.file_buffer);
		return nullptr;
	};

	// Let's assume that all servers are sane, so rather than drawing some
	// sort of line and saying "well, on *these* errors we'll give it
	// another chance, and on *these* we don't", we'll just disable a
	// server on any error.
	switch(*ret) {
	case GET_CANCELLED:
		return fail("Cancelled");
	case GET_INVALID_PARAMETER:
		// Should never really happen, but if it does, we're probably
		// offline anyway.
		return fail(NULL);
	case GET_OUT_OF_MEMORY:
		return fail("Out of memory");
	case GET_SERVER_ERROR:
		// A more detailed error was already printed by the get function.
		return fail(NULL);
	case GET_NOT_AVAILABLE:
		// We used to not disable any server in this case, but think about
		// a situation in which multiple, or even most of the files on a
		// server can't be found. We'd just be wasting time waiting for
		// each of those 404s. Maybe it might be worth it once downloads
		// are faster, but for now, it certainly isn't.
		return fail("Not Available");
	}
	assert(*ret == GET_OK);
	if(*file_size == 0) {
		// This might be a legit non-failure case one day, but until then...
		return fail("0-byte file!");
	}

	// There's not much point in putting a mutex on the time field, but
	// we should at least use a temporary variable here to ensure the
	// correct time being printed.
	auto diff_ping = ctx.time_ping - ctx.time_start;
	auto diff_transfer = ctx.time_end - ctx.time_ping;
	double diff_ping_ms = (diff_ping / perffreq) * 1000.0;
	double diff_transfer_ms = (diff_transfer / perffreq) * 1000.0;
	log_printf(
		"(%d b, %.1f + %.1f ms)\n",
		*file_size, diff_ping_ms, diff_transfer_ms
	);
	this->ping_push(diff_ping);

	if(exp_crc) {
		auto crc = crc32(0, ctx.file_buffer, ctx.file_size);
		if(*exp_crc != crc) {
			return fail("CRC32 mismatch! Please report this to server owner.");
		}
	}
	return ctx.file_buffer;
}

int servers_t::get_first() const
{
	LONGLONG last_time = LLONG_MAX;
	size_t i = 0;
	int fastest = -1;
	int tryout = -1;

	// Any verification is performed in servers::download().
	if(this->size() == 0) {
		return 0;
	}

	// Get fastest server from previous data
	for(i = 0; i < this->size(); i++) {
		const auto &server = (*this)[i];
		auto ping_average = server.ping_average();
		if(server.visited() && ping_average < last_time) {
			last_time = ping_average;
			fastest = i;
		} else if(server.unused()) {
			tryout = i;
		}
	}
	if(tryout != -1) {
		return tryout;
	} else if(fastest != -1) {
		return fastest;
	} else {
		// Everything down...
		return -1;
	}
}

int servers_t::num_active() const
{
	int ret = 0;
	for(const auto& i : (*this)) {
		if(i.active()) {
			ret++;
		}
	}
	return ret;
}

void* servers_t::download(DWORD *file_size, const char *fn, const DWORD *exp_crc, file_callback_t callback, void *callback_param)
{
	assert(file_size);
	int i;

	int servers_first = this->get_first();
	auto servers_total = this->size();
	auto servers_left = servers_total;

	if(!fn || servers_first < 0) {
		return NULL;
	}
	for(i = servers_first; servers_left; i = (i + 1) % servers_total) {
		get_result_t get_ret;
		server_t &server = (*this)[i];

		servers_left--;
		if(!server.active()) {
			continue;
		} else if(i != servers_first) {
			log_printf("Retrying on next server...\n");
		}

		auto file_buffer = server.download(file_size, &get_ret, fn, exp_crc, callback, callback_param);

		if(get_ret <= GET_INVALID_PARAMETER) {
			return NULL;
		} else if(file_buffer) {
			return file_buffer;
		}
	}
	return NULL;
}

void servers_t::from(const json_t *servers)
{
	auto validate = [] (size_t pos, json_t *server) {
		if(!json_is_string(server)) {
			char *val_str = json_dumps(server, JSON_ENCODE_ANY);
			log_printf(
				"ERROR: Expected a server string at array position %u, got \"%s\"\n",
				pos + 1, val_str
			);
			return false;
		}
		const stringref_t url = server;
		const stringref_t PROTOCOL = "://";
		int check_len = url.len - PROTOCOL.len;
		bool valid = false;
		for(decltype(check_len) i = 1; i <= check_len && !valid; i++) {
			if(!memcmp(url.str + i, PROTOCOL.str, PROTOCOL.len)) {
				valid = true;
			}
		}
		if(!valid) {
			log_printf("ERROR: not an URI: \"%s\"\n", url);
			return false;
		}
		return true;
	};

	auto servers_len = json_array_size(servers);
	for(size_t i = 0; i < servers_len; i++) {
		json_t *val = json_array_get(servers, i);
		if(validate(i, val)) {
			server_t server_new;
			server_new.url = json_string_value(val);
			server_new.new_session();
			this->push_back(server_new);
		}
	}
}

// Needs to be a global rather than a function-local static variable to
// guarantee that it's initialized correctly. Otherwise, operator[] would
// throw a "vector subscript out of range" exception if more than one thread
// called servers_cache() at roughly the same time.
// And yes, that lock is necessary.
SRWLOCK cache_srwlock = {SRWLOCK_INIT};
std::unordered_map<const json_t *, servers_t> patch_servers;

servers_t& servers_cache(const json_t *servers)
{
	AcquireSRWLockExclusive(&cache_srwlock);
	servers_t &srvs = patch_servers[servers];
	ReleaseSRWLockExclusive(&cache_srwlock);
	if(srvs.size() == 0) {
		srvs.from(servers);
	}
	return srvs;
}
/// ---------------------------------------

void* ServerDownloadFile(
	json_t *servers, const char *fn, DWORD *file_size, const DWORD *exp_crc, file_callback_t callback, void *callback_param
)
{
	return servers_cache(servers).download(file_size, fn, exp_crc, callback, callback_param);
}

int PatchFileRequiresUpdate(const json_t *patch_info, const char *fn, json_t *local_val, json_t *remote_val)
{
	// Remove if the remote specifies a JSON null,
	// but skip if the file doesn't exit locally
	if(json_is_null(remote_val)) {
		return local_val != NULL && patch_file_exists(patch_info, fn);
	}
	// Update if remote and local JSON values don't match
	if(!json_equal(local_val, remote_val)) {
		return 1;
	}
	// Update if local file doesn't exist
	if(!patch_file_exists(patch_info, fn)) {
		return 1;
	}
	return 0;
}

int update_filter_global(const char *fn, json_t *null)
{
	return strchr(fn, '/') == NULL;
}

int update_filter_games(const char *fn, json_t *games)
{
	STRLEN_DEC(fn);
	size_t i = 0;
	json_t *val;
	json_flex_array_foreach(games, i, val) {
		// We will need to match "th14", but not "th143".
		size_t val_len = json_string_length(val);
		if(
			fn_len > val_len
			&& !strnicmp(fn, json_string_value(val), val_len)
			&& fn[val_len] == '/'
		) {
			return 1;
		}
	}
	return update_filter_global(fn, NULL);
}

json_t* patch_bootstrap(const json_t *sel, json_t *repo_servers)
{
	const char *main_fn = "patch.js";
	void *patch_js_buffer;
	DWORD patch_js_size;
	json_t *patch_info = patch_build(sel);
	const json_t *patch_id = json_array_get(sel, 1);
	size_t patch_len = json_string_length(patch_id) + 1;

	size_t remote_patch_fn_len = patch_len + 1 + strlen(main_fn) + 1;
	VLA(char, remote_patch_fn, remote_patch_fn_len);
	sprintf(remote_patch_fn, "%s/%s", json_string_value(patch_id), main_fn);

	patch_js_buffer = ServerDownloadFile(repo_servers, remote_patch_fn, &patch_js_size, NULL, NULL, NULL);
	patch_file_store(patch_info, main_fn, patch_js_buffer, patch_js_size);
	// TODO: Nice, friendly error

	VLA_FREE(remote_patch_fn);
	SAFE_FREE(patch_js_buffer);
	return patch_info;
}

typedef struct {
	json_t *patch;
	DWORD patch_progress;
	DWORD patch_total;
	patch_update_callback_t callback;
	void *callback_param;
} patch_update_callback_param_t;
int patch_update_callback(const char *fn, get_result_t ret, DWORD file_progress, DWORD file_total, void *param_)
{
	patch_update_callback_param_t *param = (patch_update_callback_param_t*)param_;
	if (param->callback) {
		auto &servers = servers_cache(json_object_get(param->patch, "servers"));
		for (auto& server : servers) {
			if (strncmp(server.url, fn, strlen(server.url)) == 0) {
				fn += strlen(server.url);
				break;
			}
		}
		return param->callback(param->patch, param->patch_progress, param->patch_total, fn, ret, file_progress, file_total, param->callback_param);
	}
	return 0;
}

int patch_update(json_t *patch_info, update_filter_func_t filter_func, json_t *filter_data, patch_update_callback_t callback, void *callback_param)
{
	const char *files_fn = "files.js";

	json_t *local_files = NULL;

	DWORD remote_files_js_size;
	char *remote_files_js_buffer = NULL;

	json_t *remote_files_orig = NULL;
	json_t *remote_files_to_get = NULL;
	json_t *remote_val;

	size_t i = 0;
	size_t file_count = 0;
	size_t file_digits = 0;

	patch_update_callback_param_t patch_update_callback_param;
	patch_update_callback_param.patch = patch_info;
	patch_update_callback_param.callback = callback;
	patch_update_callback_param.callback_param = callback_param;

	const char *key;
	const char *patch_name = json_object_get_string(patch_info, "id");

	auto finish = [&] (int ret) {
		if(ret == 3) {
			log_printf("Can't reach any valid server at the moment.\nCancelling update...\n");
		}
		SAFE_FREE(remote_files_js_buffer);
		json_decref(remote_files_to_get);
		json_decref(remote_files_orig);
		json_decref(local_files);
		return ret;
	};

	auto update_delete = [&patch_info, &local_files] (const char *fn) {
		log_printf("Deleting %s...\n", fn);
		if(!patch_file_delete(patch_info, fn)) {
			json_object_del(local_files, fn);
		}
	};

	if(!patch_info) {
		return -1;
	}

	// Assuming the repo/patch hierarchy here, but if we ever support
	// repository-less Git patches, they won't be using the files.js
	// protocol anyway.
	if(patch_file_exists(patch_info, "../.git")) {
		if(patch_name) {
			log_printf("(%s is under revision control, not updating.)\n", patch_name);
		}
		return finish(1);
	}
	if(json_is_false(json_object_get(patch_info, "update"))) {
		// Updating manually deactivated on this patch
		return finish(1);
	}

	auto &servers = servers_cache(json_object_get(patch_info, "servers"));
	if(servers.size() == 0) {
		// No servers for this patch
		return finish(2);
	}

	local_files = patch_json_load(patch_info, files_fn, NULL);
	if(!json_is_object(local_files)) {
		local_files = json_object();
	}
	if(patch_name) {
		log_printf("Checking for updates of %s...\n", patch_name);
	}

	remote_files_js_buffer = (char *)servers.download(&remote_files_js_size, files_fn, NULL, NULL, NULL);
	if(!remote_files_js_buffer) {
		// All servers offline...
		return finish(3);
	}

	remote_files_orig = json_loadb_report(remote_files_js_buffer, remote_files_js_size, 0, files_fn);
	if(!json_is_object(remote_files_orig)) {
		// Remote files.js is invalid!
		return finish(4);
	}

	// Determine files to download
	remote_files_to_get = json_object();
	json_object_foreach(remote_files_orig, key, remote_val) {
		json_t *local_val = json_object_get(local_files, key);
		// Did someone simply drop a full files.js into a standalone
		// package that doesn't actually come with the files for
		// every game?
		// (Necessary in case this patch installation should later
		// cover more games. If the remote files haven't changed by
		// then, they wouldn't be downloaded if files.js pretends
		// that these versions already exist locally.)
		if(local_val && !patch_file_exists(patch_info, key)) {
			json_object_del(local_files, key);
			local_val = nullptr;
		}
		if(
			(filter_func ? filter_func(key, filter_data) : 1)
			&& PatchFileRequiresUpdate(patch_info, key, local_val, remote_val)
		) {
			json_object_set(remote_files_to_get, key, remote_val);
		}
	}
	
	file_count = json_object_size(remote_files_to_get);
	if(!file_count) {
		log_printf("Everything up-to-date.\n");
		return finish(0);
	}
	patch_update_callback_param.patch_total = file_count;
	file_digits = str_num_digits(file_count);
	log_printf("Need to get %d files.\n", file_count);

	i = 0;
	json_object_foreach(remote_files_to_get, key, remote_val) {
		void *file_buffer = nullptr;
		DWORD file_size;
		json_t *local_val;

		if(servers.num_active() == 0) {
			return finish(3);
		}

		log_printf("(%*d/%*d) ", file_digits, ++i, file_digits, file_count);
		patch_update_callback_param.patch_progress = i;
		local_val = json_object_get(local_files, key);

		// Delete locally unchanged files with a JSON null value in the remote list
		if(json_is_null(remote_val) && json_is_integer(local_val)) {
			file_size = 0;
			file_buffer = patch_file_load(patch_info, key, (size_t*)&file_size);
			if(file_buffer && file_size) {
				DWORD local_crc = crc32(0, (Bytef*)file_buffer, file_size);
				if(local_crc == json_integer_value(local_val)) {
					update_delete(key);
				} else {
					log_printf("%s (locally changed, skipping deletion)\n", key);
				}
			}
			SAFE_FREE(file_buffer);
		} else if(json_is_null(local_val)) {
			// Delete files that shouldn't exist, according to files.js.
			// Mainly intended to work around broken third-party patch
			// downloaders, since we don't even write JSON nulls to the
			// local files.js ourselves.
			update_delete(key);
		} else if(json_is_integer(remote_val)) {
			DWORD remote_crc = (DWORD)json_integer_value(remote_val);
			file_buffer = servers.download(&file_size, key, &remote_crc, patch_update_callback, &patch_update_callback_param);
		} else {
			file_buffer = servers.download(&file_size, key, NULL, patch_update_callback, &patch_update_callback_param);
		}
		if(file_buffer) {
			patch_file_store(patch_info, key, file_buffer, file_size);
			SAFE_FREE(file_buffer);
			json_object_set(local_files, key, remote_val);
		}
		patch_json_store(patch_info, files_fn, local_files);
	}
	if(i == file_count) {
		log_printf("Update completed.\n");
	}
	return finish(0);
}

typedef struct {
	DWORD stack_progress;
	DWORD stack_total;
	stack_update_callback_t callback;
	void *callback_param;
} stack_update_callback_param_t;
int stack_update_callback(const json_t *patch, DWORD patch_progress, DWORD patch_total, const char *fn, get_result_t ret, DWORD file_progress, DWORD file_total, void *param_)
{
	stack_update_callback_param_t *param = (stack_update_callback_param_t*)param_;
	if (param->callback) {
		return param->callback(param->stack_progress, param->stack_total, patch, patch_progress, patch_total, fn, ret, file_progress, file_total, param->callback_param);
	}
	return 0;
}

void stack_update(update_filter_func_t filter_func, json_t *filter_data, stack_update_callback_t callback, void *callback_param)
{
	json_t *patch_array = json_object_get(runconfig_get(), "patches");
	stack_update_callback_param_t stack_update_param;
	stack_update_param.callback = callback;
	stack_update_param.callback_param = callback_param;
	stack_update_param.stack_total = json_array_size(patch_array);
	size_t i;
	json_t *patch_info;
	json_array_foreach(patch_array, i, patch_info) {
		stack_update_param.stack_progress = i;
		patch_update(patch_info, filter_func, filter_data, stack_update_callback, &stack_update_param);
	}
}

void global_update(stack_update_callback_t callback, void *callback_param)
{
	json_t *patches = json_object();

	size_t i;
	json_t *patch;
	json_array_foreach(json_object_get(runconfig_get(), "patches"), i, patch) {
		const char *archive = json_object_get_string(patch, "archive");
		if (archive) {
			json_object_set(patches, archive, patch);
		}
	}

	WIN32_FIND_DATAA data;
	HANDLE hFind = FindFirstFile("*.js", &data);
	if (hFind == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		json_t *config = json_load_file(data.cFileName, 0, nullptr);
		if (!config) {
			continue;
		}
		json_array_foreach(json_object_get(config, "patches"), i, patch) {
			patch_rel_to_abs(patch, data.cFileName);
			patch = patch_init(patch);
			const char *archive = json_object_get_string(patch, "archive");
			if (archive && json_object_get(patches, archive) == nullptr) {
				json_object_set(patches, archive, patch);
			}
		}
		json_decref(config);
	} while (FindNextFile(hFind, &data));

	stack_update_callback_param_t stack_update_param;
	stack_update_param.callback = callback;
	stack_update_param.callback_param = callback_param;
	stack_update_param.stack_total = json_object_size(patches);

	json_t *games = json_load_file("games.js", 0, nullptr);
	if (!games) {
		json_decref(patches);
		return;
	}
	json_t *filter = json_object_get_keys_sorted(games);
	json_decref(games);

	i = 0;
	const char *key;
	json_t *patch_info;
	json_object_foreach(patches, key, patch_info) {
		stack_update_param.stack_progress = i;
		patch_update(patch_info, update_filter_games, filter, stack_update_callback, &stack_update_param);
		i++;
	}

	json_decref(filter);
	json_decref(patches);
}
