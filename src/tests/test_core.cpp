#include "test_framework.h"

#include "df/common.h"
#include "df/crc32.h"
#include "df/guid.h"
#include "df/units.h"

using namespace df;

DF_TEST(Crc32KnownVector) {
    // IEEE CRC32 of "123456789" must be 0xCBF43926
    CHECK_EQ(Crc32::Compute("123456789", 9), 0xCBF43926u);
    CHECK_EQ(Crc32::Compute("", 0), 0u);
    // incremental == one shot
    Crc32 c;
    c.Update("1234", 4);
    c.Update("56789", 5);
    CHECK_EQ(c.Final(), 0xCBF43926u);
}

DF_TEST(Crc32EmptyVsData) {
    uint8_t zeros[64] = {};
    CHECK_EQ(Crc32::Compute(zeros, 64), 0x758D6336u);  // known CRC32 of 64 zero bytes
}

DF_TEST(GuidParseFormatRoundtrip) {
    const char* s = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
    Guid g = Guid::Parse(s);
    CHECK_EQ(g.Str(), std::string(s));
    CHECK(g == Guid::GptSystemEsp());
    CHECK(Guid::Parse("not-a-guid").IsZero());
    CHECK(Guid::Parse("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}") == Guid::GptBasicData());
}

DF_TEST(GuidMixedEndian) {
    Guid g = Guid::Parse("00000000-0000-0000-0000-000000000001");
    uint8_t buf[16];
    g.WriteLE(buf);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[15], 1);
    Guid g2 = Guid::Parse("01020304-0506-0708-090A-0B0C0D0E0F10");
    g2.WriteLE(buf);
    // Data1=01020304 stored LE -> bytes 04 03 02 01
    CHECK_EQ(buf[0], 0x04);
    CHECK_EQ(buf[1], 0x03);
    CHECK_EQ(buf[2], 0x02);
    CHECK_EQ(buf[3], 0x01);
    CHECK_EQ(buf[4], 0x06);
    CHECK_EQ(buf[5], 0x05);
    CHECK_EQ(buf[6], 0x08);
    CHECK_EQ(buf[7], 0x07);
    CHECK_EQ(buf[8], 0x09);
    Guid back;
    back.ReadLE(buf);
    CHECK(back == g2);
}

DF_TEST(GuidRandomUnique) {
    Guid a = Guid::Random(), b = Guid::Random();
    CHECK(!(a == b));
    CHECK(!a.IsZero());
    // version/variant bits
    CHECK_EQ((a.b[7] >> 4) & 0xF, 0x4);
    CHECK((a.b[9] & 0xC0) == 0x80);
}

DF_TEST(UnicodeConversions) {
    std::string utf8 = "DiskForge €∆中文 \xE2\x9C\x93";
    std::u16string u16 = U8ToU16(utf8);
    CHECK_EQ(U16ToU8(u16), utf8);
    CHECK(U8ToU16("abc").size() == 3);
    CHECK_EQ(U16ToU8(u"ABC"), std::string("ABC"));
}

DF_TEST(EndianHelpers) {
    uint8_t buf[8] = {0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89};
    CHECK_EQ(Rd16(buf), 0x5678u);
    CHECK_EQ(Rd32(buf), 0x12345678u);
    CHECK_EQ(Rd64(buf), 0x89ABCDEF12345678ull);
    uint8_t out[8];
    Wr16(out, 0xAA55);
    CHECK_EQ(out[0], 0x55);
    CHECK_EQ(out[1], 0xAA);
    Wr64(out, 0x1122334455667788ull);
    CHECK_EQ(out[0], 0x88);
    CHECK_EQ(out[7], 0x11);
}

DF_TEST(SizeFormatting) {
    CHECK_EQ(FormatSize(0), std::string("0 B"));
    CHECK_EQ(FormatSize(512), std::string("512 B"));
    CHECK_EQ(FormatSize(1024), std::string("1.00 KiB"));
    CHECK_EQ(FormatSize(kMiB), std::string("1.00 MiB"));
    CHECK_EQ(FormatSize(1536 * kMiB), std::string("1.50 GiB"));
    CHECK_EQ(FormatSectors(2048), std::string("1.00 MiB"));
}

DF_TEST(SizeParsing) {
    uint64_t v = 0;
    CHECK(ParseSize("10G", &v));
    CHECK_EQ(v, 10ull * kGiB);
    CHECK(ParseSize("512MB", &v));
    CHECK_EQ(v, 512ull * kMiB);
    CHECK(ParseSize("1.5 TiB", &v));
    CHECK_EQ(v, uint64_t(1.5 * kGiB * 1024.0));
    CHECK(ParseSize("4096", &v));
    CHECK_EQ(v, 4096ull);
    CHECK(!ParseSize("abc", &v));
    CHECK(!ParseSize("-5MB", &v));
    CHECK(ParseSize("2mib", &v));
    CHECK_EQ(v, 2 * kMiB);
}

DF_TEST(Rounding) {
    CHECK_EQ(RoundUp(1, 2048), 2048ull);
    CHECK_EQ(RoundUp(2048, 2048), 2048ull);
    CHECK_EQ(RoundDown(4095, 2048), 2048ull);
    CHECK(IsPow2(4096));
    CHECK(!IsPow2(4095));
}
