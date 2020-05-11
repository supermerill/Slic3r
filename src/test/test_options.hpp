#ifndef TEST_OPTIONS_HPP
#define TEST_OPTIONS_HPP
#include <string>

/// Directory path, passed in from the outside, for the path to the test inputs dir.
constexpr char* testfile_dir {"/home/ale/GIT/Slic3r/src/test/inputs/"};

inline std::string testfile(const std::string &filename) {
    std::string result;
    result.append(testfile_dir);
    result.append(filename);
    return result;
}

#endif // TEST_OPTIONS_HPP
