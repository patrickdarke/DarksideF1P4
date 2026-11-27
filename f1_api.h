// HTTPS client for api.openf1.org (ESP-IDF esp_http_client, keep-alive,
// built-in CA bundle, OAuth bearer token when credentials are configured).
#pragma once
#include <stddef.h>
#include <stdint.h>

void apiBegin();
// GET /v1/<pathQuery>. Returns the HTTP status (200, 404, 401, 429, ...) or
// a negative number on a transport error. The body is valid until the next call.
int apiGet(const char *pathQuery);
const char *apiBody();
size_t apiLen();
bool apiHasCreds();   // an OpenF1 username is configured
bool apiAuthed();     // and a token is currently held
// Copy a token that is good for at least another few minutes, logging in
// again if need be. False without credentials or if the login failed.
bool apiToken(char *out, size_t n);
int32_t apiTokenLeftMs();   // how long the token just handed out is still good for
// GET any https URL (driver portraits). Same body buffer as apiGet().
int apiGetUrl(const char *url);
