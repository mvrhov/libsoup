#include <libsoup/soup-cookie-jar.h>
#include <libsoup/soup-cookie-jar-text.h>
#include <libsoup/soup-uri.h>
#include <gobject/gobject.h>
#include <stdio.h>

/* building libsoap for debug:
> ./configure --prefix=/code/go/src/vss/libsoup-2.52.2/build --enable-debug=yes
> make install 
*/

/* building this visionect "test app"

> gcc visionect.c -o visionect -ggdb -L ../build/lib/ -I ../build/include/libsoup-2.4/ -lsoup-2.4 `pkg-config --libs --cflags glib-2.0 gobject-2.0`

running via debugger
> LD_LIBRARY_PATH=../build/lib/ VSS_COOKIE_DB_ADDR= VSS_COOKIE_DB_PORT= VSS_COOKIE_DB_USER= VSS_COOKIE_DB_PASS= VSS_COOKIE_DB_NAME=  gdbserver :1234 visionect

*/

/* vs code configuration for debug

		{
			"name": "C++ Launch",
			"type": "cppdbg",
			"request": "launch",
			"program": "${workspaceRoot}/examples/visionect",
			"miDebuggerServerAddress": "localhost:1234",
			"cwd": "${workspaceRoot}",
			"externalConsole": true,
			"MIMode": "gdb",
			"additionalSOLibSearchPath": "${workspaceRoot}/build/lib",
			"sourceFileMap":{
				"/code/go/src/vss/libsoup-2.52.2": "${workspaceRoot}"
			}
		}
*/ 

void main() 
{
	SoupCookieJar *txt = soup_cookie_jar_text_new ("?db?/tmp/00001111-2222-3333-4444-5555deadbeef-cookies.txt", FALSE);
    //SoupCookieJar *txt = soup_cookie_jar_text_new ("?db?", FALSE);
    //SoupCookieJar *txt = soup_cookie_jar_text_new ("?db?/tmp/00001111-3333-4444-5555deadbeef-cookies.txt", FALSE);
	//SoupCookieJar *txt = soup_cookie_jar_text_new ("?db?00001111-2222-3333-4444-5555deadbeef-cookies.txt", FALSE);
	//SoupCookieJar *txt = soup_cookie_jar_text_new ("?db?00001111-2222-3333-4444-5555deadbeef.txt", FALSE);
	SoupURI *uri = soup_uri_new("http://google.com");

	char *c = soup_cookie_jar_get_cookies(txt, uri, FALSE);
	printf("cookies %s\n", c);
	//soup_cookie_jar_set_cookie(txt, uri, "test_cookie=; expires=Wed, 28 Feb 2035 18:30:53 GMT; path=/");
	soup_cookie_jar_set_cookie(txt, uri, "test_cookie1=expires_and_path; expires=Fri, 24 Mar 2130 01:01:01 GMT; path=/");
	soup_cookie_jar_set_cookie(txt, uri, "test_cookie2=secure_expires_httpOnly_path; Secure; expires=Wed, 28 Feb 2035 02:02:02 GMT; HttpOnly; path=/;");
	soup_cookie_jar_set_cookie(txt, uri, "test_cookie3=expires_secure_httpOnly_path_googleDomain; expires=Wed, 28 Feb 2035 03:03:03 GMT; Secure; HttpOnly; path=/; Domain=google.com");
	soup_cookie_jar_set_cookie(txt, uri, "test_cookie2=owerwrite; expires=Wed, 28 Feb 2035 02:02:02 GMT; path=/");
	c = soup_cookie_jar_get_cookies(txt, uri, FALSE);
	printf("cookies %s\n", c);

	g_object_unref (txt);
}

