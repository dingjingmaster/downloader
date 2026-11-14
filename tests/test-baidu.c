#define _GNU_SOURCE

#include "baidu-parse.h"
#include "baidu-sign.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void test_sign_vector (void)
{
    char sign[128];

    assert (baidu_compute_sign ("e8c7d729eea7b54551aa594f942decbe",
                                "37dbe07ade9359c1aa70807e847f768c13360ad2",
                                sign, sizeof (sign)) == 0);
    assert (strcmp (sign, "8RxCbsVeSzn2UjxJAAiV9QQs/WetOj2FJUGwjsMG6SgxFMWlLS/U1Q==") == 0);
}

static void test_page_sign (void)
{
    char   sign1[64];
    char   sign3[64];
    char   timestamp[32];
    char   html[] = "xx\"sign1\":\"AAA\"yy\"sign3\":\"BBB\",\"timestamp\":1690000000,zz";

    assert (baidu_page_extract_sign (html, sign1, sizeof (sign1), sign3, sizeof (sign3),
                                     timestamp, sizeof (timestamp)) == 0);
    assert (strcmp (sign1, "AAA") == 0);
    assert (strcmp (sign3, "BBB") == 0);
    assert (strcmp (timestamp, "1690000000") == 0);
    errno = 0;
    assert (baidu_page_extract_sign ("no sign here", sign1, sizeof (sign1), sign3, sizeof (sign3),
                                     timestamp, sizeof (timestamp)) == -1);
    assert (errno == EPROTO);
}

static void test_share_url (void)
{
    char surl[BAIDU_SURL_SIZE];
    char pwd[16];

    assert (baidu_share_url ("https://pan.baidu.com/s/1AbCdEf", surl, sizeof (surl)));
    assert (strcmp (surl, "1AbCdEf") == 0);
    assert (baidu_share_url ("https://yun.baidu.com/s/1AbCdEf", surl, sizeof (surl)));
    assert (strcmp (surl, "1AbCdEf") == 0);
    assert (baidu_share_url ("https://pan.baidu.com/share/init?surl=1AbCdEf", surl, sizeof (surl)));
    assert (strcmp (surl, "1AbCdEf") == 0);
    assert (!baidu_share_url ("https://pan.baidu.com/disk/home", surl, sizeof (surl)));
    assert (!baidu_share_url ("https://example.test/s/1AbCdEf", surl, sizeof (surl)));

    assert (baidu_share_url_pwd ("https://pan.baidu.com/s/1AbCdEf?pwd=abcd", pwd, sizeof (pwd)) == 0);
    assert (strcmp (pwd, "abcd") == 0);
    assert (baidu_share_url_pwd ("https://pan.baidu.com/s/1AbCdEf", pwd, sizeof (pwd)) != 0);
}

static void test_own_url (void)
{
    char path[DOWNLOAD_TASK_PATH_SIZE];

    assert (baidu_own_url ("baidu:///%E8%B5%84%E6%96%99/a.bin", path, sizeof (path)));
    assert (strcmp (path, "/资料/a.bin") == 0);
    assert (!baidu_own_url ("baidu:///../etc/passwd", path, sizeof (path)));
    assert (!baidu_own_url ("baidu:///", path, sizeof (path)));
    assert (!baidu_own_url ("https://pan.baidu.com/s/1AbCdEf", path, sizeof (path)));
}

static void test_options (void)
{
    BaiduTaskOptions options;

    assert (baidu_task_options ("{\"baidu\":{\"pwd\":\"abcd\",\"path\":\"/资料\"}}", &options) == 0);
    assert (strcmp (options.pwd, "abcd") == 0);
    assert (strcmp (options.path, "/资料") == 0);
    assert (baidu_task_options ("{}", &options) == 0);
    assert (options.pwd[0] == '\0');
    assert (baidu_task_options ("{\"baidu\":", &options) != 0);
}

static void test_cookie_login (void)
{
    assert (baidu_cookie_login ("BDUSS=x; STOKEN=y") == 0);
    assert (baidu_cookie_login ("PANWEB=1; BDUSS=x; STOKEN=y; SCRC=z") == 0);
    assert (baidu_cookie_login ("BDUSS=x") != 0);
    assert (baidu_cookie_login ("STOKEN=y") != 0);
    assert (baidu_cookie_login ("BDUSS=; STOKEN=y") != 0);
}

static void test_share_page_tokens (void)
{
    BaiduShareTokens tokens;
    char page[] = "{\"loginstate\":1,\"bdstoken\":\"tok\",\"uk\":\"111\","
                  "\"share_uk\":\"222\",\"shareid\":\"333\"}";

    assert (baidu_share_page_tokens (page, &tokens) == 0);
    assert (strcmp (tokens.bdstoken, "tok") == 0);
    assert (strcmp (tokens.uk, "111") == 0);
    assert (strcmp (tokens.shareUk, "222") == 0);
    assert (strcmp (tokens.shareid, "333") == 0);

    errno = 0;
    assert (baidu_share_page_tokens ("<html>platform-non-found</html>", &tokens) == -1);
    assert (errno == ESTALE);
    errno = 0;
    assert (baidu_share_page_tokens ("<html>error-404</html>", &tokens) == -1);
    assert (errno == ENOENT);
    errno = 0;
    assert (baidu_share_page_tokens ("<html>nothing</html>", &tokens) == -1);
    assert (errno == EPROTO);
}

int main (void)
{
    test_sign_vector ();
    test_page_sign ();
    test_share_url ();
    test_own_url ();
    test_options ();
    test_cookie_login ();
    test_share_page_tokens ();
    return 0;
}
