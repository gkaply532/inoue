#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <curl/curl.h>
#include "json.h"
#include "winunistd.h"

#include "inoue.h"

struct _cfg config = {0};

struct json_object_s *
json_get_api_data(struct json_value_s *root)
{
	struct json_object_s *root_obj = json_value_as_object(root);
	if (!root_obj)
		return NULL;
	for (struct json_object_element_s *i = root_obj->start; i != NULL; i = i->next) {
		if (0 == strcmp(i->name->string, "success")) {
			if (!json_value_is_true(i->value))
				return NULL;
		} else if (0 == strcmp(i->name->string, "data")) {
			return json_value_as_object(i->value);
		}
	}
	return NULL;
}

int
parse_userid(const char *apiresp, size_t apiresplen, char *userid)
{
	int ret = 1;
	struct json_value_s *root = json_parse(apiresp, apiresplen);
	struct json_object_s *data = json_get_api_data(root);
	if (data) {
		struct json_value_s *idv = json_getpath(data, "user._id");
		struct json_string_s *id = json_value_as_string(idv);
		if (id) {
			ret = 0;
			strcpy(userid, id->string);
		}
	}
	free(root);
	return ret;
}

typedef struct {
	char opponent[32];
	char replayid[32];
	struct tm ts;
} game;

int
parse_game(struct json_object_s *json, game *game)
{
	if (!json)
		return 0;

	struct json_value_s *v = json_getpath(json, "replayid");
	if (!v)
		return 0;
	struct json_string_s *replayid = json_value_as_string(v);
	if (!replayid)
		return 0;
	strcpy(game->replayid, replayid->string);

	v = json_getpath(json, "ts");
	if (!v)
		return 0;
	struct json_string_s *ts = json_value_as_string(v);
	if (!ts)
		return 0;
	memset(&game->ts, 0, sizeof(struct tm));
	if (!parse_ts(&game->ts, ts->string)) {
		return 0;
	}

	v = json_getpath(json, "endcontext");
	struct json_array_s *endcontext = json_value_as_array(v);
	if (endcontext) {
		for (struct json_array_element_s *i = endcontext->start; i != NULL; i = i->next) {
			struct json_object_s *c = json_value_as_object(i->value);
			if (!c)
				return 0;
			v = json_getpath(c, "user.username");
			if (!v)
				return 0;
			struct json_string_s *username = json_value_as_string(v);
			if (!username)
				return 0;
			if (0 != strcmp(username->string, config.username)) {
				strcpy(game->opponent, username->string);
				break;
			}
		}
	}
	return 1;
}

int
parse_game_list(const char *apiresp, size_t apiresplen, game *games)
{
	struct json_parse_result_s result;
	struct json_value_s *root = json_parse_ex(apiresp, apiresplen, 0, NULL, NULL, &result);
	if (!root)
		return 0;
	struct json_object_s *data = json_get_api_data(root);
	int count = 0;
	if (!data) {
		free(root);
		return 0;
	}
	struct json_value_s *rv = json_getpath(data, "records");
	struct json_array_s *records = json_value_as_array(rv);
	if (!records)
		return count;
	for (struct json_array_element_s *j = records->start; j != NULL; j = j->next) {
		count += parse_game(json_value_as_object(j->value), &games[count]);
	}
	free(root);
	return count;
}

int
save_game_to_file(const char *resp, size_t len, FILE *f)
{
	if (len < 1) // sanity check
		return 0;
	size_t written = fwrite(resp, 1, len, f);
	if (written == len) {
		return 1;
	}
	perror("failed to save");
	return 0;
}

const char *
generate_filename(game *g)
{
	static buffer *buf = NULL;
	char tmp[32];
	char fmt[] = "%_";
	if (!buf)
		buf = buffer_new();
	if (!buf)
		return NULL;
	buffer_truncate(buf);
	for (int i = 0; config.filenameformat[i] != 0; i++) {
		if (config.filenameformat[i] == '%') {
			i++;
			switch(config.filenameformat[i]) {
			case 'Y':
			case 'y':
			case 'm':
			case 'd':
			case 'H':
			case 'M':
			case 'S':
			case 's':
				fmt[1] = config.filenameformat[i];
				strftime(tmp, 32, fmt, &g->ts);
				buffer_appendstr(buf, tmp);
				break;
			case 'o':
			case 'O':
				strncpy(tmp, g->opponent, 32);
				for (int j = 0; tmp[j] != 0; j++) {
					if (config.filenameformat[i] == 'o')
						tmp[j] = tolower(tmp[j]);
					else
						tmp[j] = toupper(tmp[j]);
				}
				buffer_appendstr(buf, tmp);
				break;
			case 'u':
				buffer_appendstr(buf, config.username);
				break;
			case 'U':
				strncpy(tmp, config.username, 32);
				for (int j = 0; tmp[j] != 0; j++) {
					tmp[j] = toupper(tmp[j]);
				}
				buffer_appendstr(buf, tmp);
				break;
			case 'r':
				buffer_appendstr(buf, g->replayid);
				break;
			case '%':
				buffer_appendchar(buf, '%');
				break;
			default:
				return NULL;
			}
		} else {
			buffer_appendchar(buf, config.filenameformat[i]);
		}
	}
	return buffer_str(buf);
}

size_t
recv_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	return buffer_appendbytes((buffer *)userdata, ptr, size*nmemb);
}

int
main(int argc, char **argv)
{
	printf("INOUE v0.3\n");
	printf("curl ver: %s\n", curl_version());
	int exitcode = EXIT_FAILURE;

	if (argc == 2) {
		if (chdir(argv[1]) < 0) {
			perror("inoue: couldn't change directory");
			return EXIT_FAILURE;
		}
	}

	if (!loadcfg()) {
		fprintf(stderr, "Configuration error, exiting!\n");
		return EXIT_FAILURE;
	}

	CURLcode ret;
	CURL *hnd;
	buffer *buf = buffer_new();

	curl_global_init(CURL_GLOBAL_ALL);
	hnd = curl_easy_init();
	curl_easy_setopt(hnd, CURLOPT_USERAGENT, config.useragent);
	curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, recv_callback);
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, buf);
	// curl_easy_setopt(hnd, CURLOPT_FAILONERROR, 1);

	puts("Resolving username...");
	char url_buf[128];
	snprintf(url_buf, 128, "https://ch.tetr.io/api/users/%s", config.username);
	curl_easy_setopt(hnd, CURLOPT_URL, url_buf);
	ret = curl_easy_perform(hnd);

	if (ret != CURLE_OK) {
		fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(ret));
		goto main_cleanup;
	}

	char userid[64];
	if (parse_userid(buffer_str(buf), buffer_strlen(buf), userid)) {
		fprintf(stderr, "Invalid server response while resolving username!\n");
		goto main_cleanup;
	}
	buffer_truncate(buf);

	printf("Resolved UserID: '%s'\n", userid);
	snprintf(url_buf, 128, "https://ch.tetr.io/api/streams/league_userrecent_%s", userid);
	curl_easy_setopt(hnd, CURLOPT_URL, url_buf);
	ret = curl_easy_perform(hnd);
	if (ret != CURLE_OK) {
		fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(ret));
		goto main_cleanup;
	}

	game games[10];
	int gamec = parse_game_list(buffer_str(buf), buffer_strlen(buf), games); // FIXME: fails
	if (!gamec) {
		fprintf(stderr, "Error while parsing games, exiting!\n");
		goto main_cleanup;
	}
	for (int i = 0; i < gamec; i++) {
		char ts[64];
		strftime(ts, 64, "%Y-%m-%d %H:%M", &games[i].ts);
		printf("Found game '%s' against '%s', played on %s\n", games[i].replayid, games[i].opponent, ts);
	}

	for (int i = 0; i < gamec; i++) {
		const char *filename = generate_filename(&games[i]);
		if(access(filename, F_OK) != -1 ) {
			printf("Game %s already saved, skipping...\n", games[i].replayid);
			continue;
		}
		FILE *f = fopen(filename, "w");
		if (!f) {
			perror("inoue: couldnt open output file");
			goto main_cleanup;
		}
		snprintf(url_buf, 128, config.apiurl, games[i].replayid);
		curl_easy_setopt(hnd, CURLOPT_URL, url_buf);
		buffer_truncate(buf);
		printf("Downloading %s... ", games[i].replayid);
		fflush(stdout);
		ret = curl_easy_perform(hnd);
		if (ret != CURLE_OK) {
			fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(ret));
			fclose(f);
			unlink(filename); // delete the failed file, so the download can be retried
			goto main_cleanup;
		}
		long status;
		curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &status);
		if (status != 200) {
			fprintf(stderr, "received error %ld from server: %s\n", status, buffer_str(buf));
			fclose(f);
			unlink(filename);
			continue;
		}
		if (!save_game_to_file(buffer_str(buf), buffer_strlen(buf), f)) {
			fprintf(stderr, "Saving failed!\n");
			fclose(f);
			unlink(filename);
			continue;
		}
		fclose(f);
		printf("OK!\n");

	}

	// if the program is exited via goto, then it will return FAILURE instead
	exitcode = EXIT_SUCCESS;
main_cleanup:
	curl_easy_cleanup(hnd);
	buffer_free(buf);

	return exitcode;
}
