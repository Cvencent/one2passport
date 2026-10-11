#include "radio_mp3_header.h"

#include <cassert>

int main() {
    assert(radio_mp3_header_valid(0xff, 0xfb, 0x90));
    assert(radio_mp3_header_valid(0xff, 0xf3, 0x80));
    assert(radio_mp3_header_valid(0xff, 0xe3, 0x80));
    assert(!radio_mp3_header_valid(0xff, 0xfd, 0x90)); // Layer II
    assert(!radio_mp3_header_valid(0xff, 0xff, 0x90)); // Layer I
    assert(!radio_mp3_header_valid(0xff, 0xeb, 0x90)); // Reserved version
    assert(!radio_mp3_header_valid(0xff, 0xfb, 0xfc)); // Reserved bitrate/rate
    assert(!radio_mp3_header_valid(0xff, 0xfb, 0x0c));
    assert(!radio_mp3_header_valid(0xff, 0xdb, 0x90)); // False sync
}
