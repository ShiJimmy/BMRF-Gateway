#include "test_framework.hpp"

#include "bmrf/capture/capture.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace bmrf;

BMRF_TEST(capture_write_read_round_trip)
{
    const std::string path = "bmrf_capture_test.txt";
    std::remove(path.c_str());

    std::string error;
    BMRF_CHECK(captureStart(path, error));
    BMRF_CHECK(captureActive());

    const uint8_t dmrd[3] = {0x01, 0xAB, 0xFF};
    captureWrite("bm_rx", 1234, dmrd, sizeof(dmrd));
    captureWriteText("rf_frame", 5678, "src=1 dst=2 seq=3");
    captureStop();
    BMRF_CHECK(!captureActive());

    std::ifstream in(path);
    BMRF_CHECK(in.good());
    std::string line1;
    std::string line2;
    std::getline(in, line1);
    std::getline(in, line2);
    in.close();

    BMRF_CHECK_EQ(line1, std::string("bm_rx 1234 01abff"));
    BMRF_CHECK_EQ(line2, std::string("rf_frame 5678 src=1 dst=2 seq=3"));

    std::remove(path.c_str());
}
