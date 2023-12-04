#include <stdio.h>

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace std;

extern "C" {

static map<char *, map<string, vector<bool>>> __replay_coverage_info;

void __record_bb_cov(char *file_name, char *func_name, size_t bb_index) {
  if (__replay_coverage_info.find(file_name) == __replay_coverage_info.end()) {
    __replay_coverage_info.insert(
        make_pair(file_name, map<string, vector<bool>>()));
  }

  map<string, vector<bool>> &file_info = __replay_coverage_info[file_name];
  if (file_info.find(func_name) == file_info.end()) {
    file_info.insert(make_pair(func_name, vector<bool>()));
  }

  vector<bool> &func_info = file_info[func_name];

  if (func_info.size() <= bb_index) {
    func_info.resize(bb_index + 1, false);
  }

  func_info[bb_index] = true;

  return;
}

// Save the converage info to each .cov file.
// This function is called when the program exits.
void __cov_fini() {
  for (auto [filename, file_info] : __replay_coverage_info) {
    // iter is pair of filename and funcname -> bb_index -> iscovered
    const string cov_file_name = string(filename) + ".cov";
    ifstream cov_file_in(cov_file_name, ios::in);

    // Load existing coverage file to file_info
    // Basically, this will do OR operation between existing coverage and new
    if (cov_file_in.is_open()) {
      string cur_func = "";

      // Read each line of coverage file
      string line;
      while (getline(cov_file_in, line)) {
        size_t first_empty = line.find(" ");

        if (first_empty == string::npos) {
          break;
        }

        size_t last_empty = line.find_last_of(" ");

        if (last_empty == string::npos) {
          break;
        }

        if (first_empty == last_empty) {
          break;
        }

        // Either 'F' or 'b'
        bool is_func = line.substr(0, first_empty) == "F";

        if (is_func) {
          string name =
              line.substr(first_empty + 1, last_empty - first_empty - 1);
          if (file_info.find(name) == file_info.end()) {
            file_info.insert(make_pair(name, vector<bool>()));
          }
          cur_func = name;
        } else {
          string index_str =
              line.substr(first_empty + 1, last_empty - first_empty - 1);
          size_t bb_index = stoul(index_str);
          bool is_covered = line.substr(last_empty + 1) == "1";
          if (file_info[cur_func].size() <= bb_index) {
            file_info[cur_func].resize(bb_index + 1, false);
          }
          if (is_covered) {
            file_info[cur_func][bb_index] = true;
          }
        }
      }  // line ends
    }
    cov_file_in.close();

    // Write the coverage info to the file
    ofstream cov_file_out(cov_file_name, ios::out);

    for (auto [func_name, func_bb_cov] : file_info) {
      bool is_func_covered = false;

      for (size_t bb_index = 0; bb_index < func_bb_cov.size(); bb_index++) {
        if (func_bb_cov[bb_index]) {
          is_func_covered = true;
          break;
        }
      }

      cov_file_out << "F " << func_name << " " << is_func_covered << "\n";

      for (size_t bb_index = 0; bb_index < func_bb_cov.size(); bb_index++) {
        cov_file_out << "B " << bb_index << " " << func_bb_cov[bb_index]
                     << "\n";
      }
    }
    cov_file_out.close();
  }

  return;
}

}  // extern "C"