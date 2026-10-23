// Custom Radio Mix courses (src/course_builder.h).
#include "../src/course_builder.h"
#include <cassert>
#include <cstdio>
#include <set>

int main() {
    // 12 songs at level 3, 8 at level 10 (one of them with a second chart in the band), 3 at 18.
    std::vector<CourseCandidate> c;
    for (size_t i = 0; i < 12; ++i) c.push_back({"bms1" + std::to_string(i), 3, i});
    for (size_t i = 0; i < 8; ++i) c.push_back({"bms2" + std::to_string(i) + "-hd", 9 + (int)(i % 3), 100 + i});
    c.push_back({"bms20-ex", 12, 100});
    for (size_t i = 0; i < 3; ++i) c.push_back({"bms3" + std::to_string(i) + "-ex", 18, 200 + i});

    const auto courses = BuildCourses(c, "bmsc5_", 10, 1337);
    // Band 1-4 has 12 songs = 3 courses, band 9-12 has 8 = 2, band 17-20 has 3 = none.
    assert(courses.size() == 5);
    int low = 0, mid = 0;
    for (const auto& d : courses) {
        assert(d.stages.size() == 4 && d.songs.size() == 4);
        assert(std::set<size_t>(d.songs.begin(), d.songs.end()).size() == 4);   // no song twice
        if (d.low == 1) ++low;
        if (d.low == 9) ++mid;
        assert(d.low != 17);
    }
    assert(low == 3 && mid == 2);
    // Bands take turns, names are numbered in order, stages climb.
    assert(courses[0].low == 1 && courses[1].low == 9 && courses[2].low == 1);
    assert(courses[0].name == "bmsc5_01" && courses[4].name == "bmsc5_05" && courses[4].number == 5);
    for (const auto& d : courses) assert(d.level == d.high || d.low == 1 || d.level <= d.high);
    // No song repeats inside a band.
    std::set<size_t> lowSongs;
    for (const auto& d : courses) if (d.low == 1) for (size_t s : d.songs) assert(lowSongs.insert(s).second);
    // The count caps, and the same seed gives the same courses.
    assert(BuildCourses(c, "x", 2, 1337).size() == 2);
    const auto again = BuildCourses(c, "bmsc5_", 10, 1337);
    for (size_t i = 0; i < courses.size(); ++i) assert(again[i].stages == courses[i].stages);

    // Stage.ini: the names join channel_normal and each course gets its list and level.
    const std::string native = "\"channel_normal\"=\"5d;random4\"\r\n\r\n\"5d\"=\"a;b;c;d\"\r\n\"5d_Level\"=\"16\"\r\n";
    const std::string text = AppendCoursesToStageIni(native, {courses[0]});
    assert(text.find("\"channel_normal\"=\"5d;random4;bmsc5_01\"") == 0);
    assert(text.find("\"bmsc5_01\"=\"" + courses[0].stages[0] + ";") != std::string::npos);
    assert(text.find("\"bmsc5_01_Level\"=\"") != std::string::npos);
    assert(text.find("\"5d\"=\"a;b;c;d\"") != std::string::npos);
    assert(AppendCoursesToStageIni("no list here", {courses[0]}) == "no list here");
    std::puts("course_builder_test OK");
    return 0;
}
