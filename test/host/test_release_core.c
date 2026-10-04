#include "test.h"
#include "release_core.h"

void test_release_core(void)
{
    int v[3];

    /* semver_parse: plain X.Y.Z only. */
    CHECK(semver_parse("1.2.3", v) && v[0] == 1 && v[1] == 2 && v[2] == 3);
    CHECK(semver_parse("10.20.30", v) && v[0] == 10 && v[2] == 30);
    CHECK(semver_parse("0.0.0", v));
    CHECK(!semver_parse("v1.2.3", v));
    CHECK(!semver_parse("1.2", v));
    CHECK(!semver_parse("1.2.3-rc1", v));
    CHECK(!semver_parse("1.2.3.4", v));
    CHECK(!semver_parse("1.2.3 ", v));
    CHECK(!semver_parse(" 1.2.3", v));
    CHECK(!semver_parse("-1.2.3", v));
    CHECK(!semver_parse("a.b.c", v));
    CHECK(!semver_parse("", v));
    CHECK(!semver_parse(NULL, v));

    /* semver_newer: numeric, strict, and false whenever either side is junk. */
    CHECK(semver_newer("1.3.0", "1.2.9"));
    CHECK(semver_newer("1.10.0", "1.9.0"));          /* not a string compare */
    CHECK(semver_newer("2.0.0", "1.99.99"));
    CHECK(semver_newer("1.4.1", "1.4.0"));
    CHECK(!semver_newer("1.4.0", "1.4.0"));
    CHECK(!semver_newer("1.2.0", "1.3.0"));
    CHECK(!semver_newer("1.9.0", "1.10.0"));
    CHECK(!semver_newer("garbage", "1.0.0"));
    CHECK(!semver_newer("9.9.9", "dev"));

    /* release_extract_tag: the shape GitHub actually sends (compact). */
    char t[16];
    const char *real =
        "{\"url\":\"https://api.github.com/repos/SEspe/BirdBox/releases/1\","
        "\"author\":{\"login\":\"SEspe\",\"id\":1},\"node_id\":\"RE_x\","
        "\"tag_name\":\"v1.4.1\",\"target_commitish\":\"master\","
        "\"assets\":[{\"name\":\"BirdBox_esp32s3_v1.4.1.bin\","
        "\"browser_download_url\":\"https://github.com/SEspe/BirdBox/releases/download/v1.4.1/BirdBox_esp32s3_v1.4.1.bin\"}],"
        "\"body\":\"notes\"}";
    CHECK(release_extract_tag(real, t, sizeof(t)));
    CHECK_STR(t, "1.4.1");
    CHECK(release_extract_tag("{\"tag_name\" : \"V2.0.0\"}", t, sizeof(t)));   /* spaces, capital V */
    CHECK_STR(t, "2.0.0");
    CHECK(release_extract_tag("{\"tag_name\":\"1.0.0\"}", t, sizeof(t)));      /* no v at all */
    CHECK_STR(t, "1.0.0");
    CHECK(!release_extract_tag("{\"name\":\"Release v1.0.0\"}", t, sizeof(t)));
    CHECK_STR(t, "");                                   /* never leaves junk behind */
    CHECK(!release_extract_tag("{\"tag_name\":\"v1.4.1-beta\"}", t, sizeof(t)));
    CHECK(!release_extract_tag("{\"tag_name\":\"\"}", t, sizeof(t)));
    CHECK(!release_extract_tag("{\"tag_name\":\"v1.4.1", t, sizeof(t)));      /* truncated reply */
    CHECK(!release_extract_tag("{\"tag_name\":v1.4.1}", t, sizeof(t)));       /* not a string */
    CHECK(!release_extract_tag("{\"tag_name\":\"<script>\"}", t, sizeof(t))); /* JSON-safe by construction */
    char small[6];
    CHECK(!release_extract_tag("{\"tag_name\":\"v1.22.333\"}", small, sizeof(small)));   /* would truncate */
    CHECK(release_extract_tag("{\"tag_name\":\"v1.2.3\"}", small, sizeof(small)));       /* exactly fits */
    CHECK_STR(small, "1.2.3");
    CHECK(!release_extract_tag(NULL, t, sizeof(t)));

    /* release_has_bin: only a .bin inside "assets" counts. */
    CHECK(release_has_bin(real));
    CHECK(!release_has_bin("{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"src.zip\"}]}"));
    CHECK(!release_has_bin("{\"tag_name\":\"v1\",\"assets\":[]}"));
    CHECK(!release_has_bin("{\"note\":\"see x.bin\\\"\",\"tag_name\":\"v1\"}"));   /* no assets key */
    CHECK(!release_has_bin(NULL));
}
