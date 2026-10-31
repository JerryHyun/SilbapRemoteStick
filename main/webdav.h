#pragma once
/* WebDAV (class 1 and 2 with fake locks) at /dav: one HTTP_ANY wildcard route behind the web middleware (WEB_AUTH),
 * plus a public OPTIONS on "/" for the Windows WebClient's first probe. Call from web_start after the server runs. */
void webdav_register(void);
