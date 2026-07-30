#include <ROOT/RDataFrame.hxx>
#include <TFile.h>
#include <TTree.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fnmatch.h>
#include <glob.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Config {
  unsigned int threads = 1;
  unsigned int processes = 1;
  fs::path inputBaseDir = "/data2/common/NanoAOD";
  fs::path outputBaseDir = "/data2/common/skimmed_NanoAOD";
  fs::path scratchDir = "/scratch";
  std::uintmax_t scratchFlushBytes = 50ULL * 1024ULL * 1024ULL * 1024ULL;
  std::vector<std::string> inputDirectories;
  std::vector<std::string> branches;
  std::vector<std::string> hltPaths;
  std::size_t progressEveryFiles = 100;
  std::string treeName = "Events";
};

class Logger {
public:
  explicit Logger(const fs::path &path) : path_(path) {
    std::ofstream file(path_, std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot open log file: " + path.string());
  }

  template <typename... Args> void info(Args &&...args) { write("INFO", std::forward<Args>(args)...); }
  template <typename... Args> void warning(Args &&...args) { write("WARNING", std::forward<Args>(args)...); }
  template <typename... Args> void error(Args &&...args) { write("ERROR", std::forward<Args>(args)...); }

private:
  template <typename... Args> void write(const char *level, Args &&...args) {
    std::ostringstream message;
    (message << ... << args);
    const auto line = timestampNow() + " [" + level + "] " + message.str();
    std::cout << line << std::endl;
    std::ofstream file(path_, std::ios::app);
    file << line << '\n';
  }

  static std::string timestampNow() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
  }

  fs::path path_;
};

std::string runTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::ostringstream os;
  os << std::put_time(&tm, "%Y%m%d_%H%M%S");
  return os.str();
}

std::vector<std::string> getStringArray(const json &j, const char *key, bool required = true) {
  if (!j.contains(key)) {
    if (required) throw std::runtime_error(std::string("Missing required JSON key: ") + key);
    return {};
  }
  if (!j.at(key).is_array()) throw std::runtime_error(std::string("JSON key must be an array: ") + key);
  return j.at(key).get<std::vector<std::string>>();
}

Config loadConfig(const fs::path &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot open config JSON: " + path.string());
  json j;
  input >> j;

  Config cfg;
  cfg.threads = j.value("threads", cfg.threads);
  cfg.processes = j.value("processes", cfg.processes);
  if (cfg.processes == 0) throw std::runtime_error("processes must be at least 1");
  cfg.inputBaseDir = j.value("input_base_directory", cfg.inputBaseDir.string());
  cfg.outputBaseDir = j.value("output_base_directory", cfg.outputBaseDir.string());
  cfg.scratchDir = j.value("scratch_directory", cfg.scratchDir.string());
  cfg.scratchFlushBytes = j.value("scratch_flush_bytes", cfg.scratchFlushBytes);
  cfg.inputDirectories = getStringArray(j, "input_directories");
  cfg.branches = getStringArray(j, "branches", false);
  cfg.hltPaths = getStringArray(j, "hlt_paths", false);
  cfg.progressEveryFiles = j.value("progress_every_files", cfg.progressEveryFiles);
  cfg.treeName = j.value("tree_name", cfg.treeName);
  return cfg;
}

std::vector<fs::path> expandInputDirectories(const Config &cfg) {
  std::set<fs::path> dirs;
  for (const auto &pattern : cfg.inputDirectories) {
    const fs::path fullPattern = cfg.inputBaseDir / pattern;
    glob_t globResult{};
    const int status = glob(fullPattern.c_str(), GLOB_TILDE, nullptr, &globResult);
    if (status == 0) {
      for (std::size_t i = 0; i < globResult.gl_pathc; ++i) {
        fs::path p = fs::path(globResult.gl_pathv[i]);
        if (fs::is_directory(p)) dirs.insert(fs::weakly_canonical(p));
      }
    } else if (status == GLOB_NOMATCH) {
      fs::path p = fullPattern;
      if (fs::is_directory(p)) dirs.insert(fs::weakly_canonical(p));
    } else {
      globfree(&globResult);
      throw std::runtime_error("glob failed for pattern: " + fullPattern.string());
    }
    globfree(&globResult);
  }
  return {dirs.begin(), dirs.end()};
}

std::vector<fs::path> collectRootFiles(const std::vector<fs::path> &directories) {
  std::vector<fs::path> files;
  for (const auto &dir : directories) {
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".root") files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<std::string> treeBranches(TTree &tree) {
  std::vector<std::string> names;
  const auto branches = tree.GetListOfBranches();
  names.reserve(branches->GetEntries());
  for (int i = 0; i < branches->GetEntries(); ++i) names.emplace_back(branches->At(i)->GetName());
  return names;
}

bool hasWildcard(const std::string &pattern) {
  return pattern.find_first_of("*?[") != std::string::npos;
}

std::vector<std::string> presentFromRequestedPatterns(const std::vector<std::string> &requested,
                                                      const std::vector<std::string> &available,
                                                      std::vector<std::string> &missingPatterns) {
  std::vector<std::string> present;
  std::set<std::string> seen;
  const std::set<std::string> availableSet(available.begin(), available.end());

  for (const auto &pattern : requested) {
    std::vector<std::string> matches;
    if (hasWildcard(pattern)) {
      for (const auto &name : available) {
        if (fnmatch(pattern.c_str(), name.c_str(), 0) == 0) matches.push_back(name);
      }
    } else if (availableSet.count(pattern)) {
      matches.push_back(pattern);
    }

    if (matches.empty()) {
      missingPatterns.push_back(pattern);
      continue;
    }

    for (const auto &match : matches) {
      if (seen.insert(match).second) present.push_back(match);
    }
  }

  return present;
}

std::string join(const std::vector<std::string> &items, const std::string &sep) {
  std::ostringstream os;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) os << sep;
    os << items[i];
  }
  return os.str();
}

fs::path relativeToBase(const fs::path &path, const fs::path &base) {
  std::error_code ec;
  auto rel = fs::relative(path, base, ec);
  if (ec) throw std::runtime_error("Cannot compute relative path for " + path.string() + " from " + base.string());
  return rel;
}

void flushScratch(std::vector<std::pair<fs::path, fs::path>> &pending, Logger &log) {
  if (pending.empty()) return;
  log.info("Flushing ", pending.size(), " skimmed file(s) from scratch to final output");
  for (const auto &[scratch, finalPath] : pending) {
    fs::create_directories(finalPath.parent_path());
    std::error_code ec;
    fs::rename(scratch, finalPath, ec);
    if (ec) {
      fs::copy_file(scratch, finalPath, fs::copy_options::overwrite_existing);
      fs::remove(scratch);
    }
  }
  pending.clear();
}

std::uintmax_t pendingBytes(const std::vector<std::pair<fs::path, fs::path>> &pending) {
  std::uintmax_t total = 0;
  for (const auto &[scratch, _] : pending) {
    std::error_code ec;
    const auto size = fs::file_size(scratch, ec);
    if (!ec) total += size;
  }
  return total;
}

void enableImplicitMTOnce(unsigned int threads) {
  static bool enabled = false;
  if (!enabled && threads > 1) {
    ROOT::EnableImplicitMT(threads);
    enabled = true;
  }
}

void skimOneFile(const Config &cfg, const fs::path &inputFile, const fs::path &scratchFile,
                 Logger &log) {
  enableImplicitMTOnce(cfg.threads);
  std::unique_ptr<TFile> file(TFile::Open(inputFile.c_str(), "READ"));
  if (!file || file->IsZombie()) throw std::runtime_error("Cannot open ROOT file: " + inputFile.string());
  auto *tree = dynamic_cast<TTree *>(file->Get(cfg.treeName.c_str()));
  if (!tree) throw std::runtime_error("Cannot find tree '" + cfg.treeName + "' in " + inputFile.string());

  const auto available = treeBranches(*tree);
  std::vector<std::string> missingBranchPatterns, missingHltPatterns;
  auto keptBranches = presentFromRequestedPatterns(cfg.branches, available, missingBranchPatterns);
  auto presentHlt = presentFromRequestedPatterns(cfg.hltPaths, available, missingHltPatterns);

  if (!missingBranchPatterns.empty()) log.warning(inputFile, " missing branch pattern(s): ", join(missingBranchPatterns, ", "));
  if (!missingHltPatterns.empty()) log.warning(inputFile, " missing HLT pattern(s): ", join(missingHltPatterns, ", "));

  for (const auto &hlt : presentHlt) {
    if (std::find(keptBranches.begin(), keptBranches.end(), hlt) == keptBranches.end()) keptBranches.push_back(hlt);
  }
  const bool hasGenWeight = std::find(available.begin(), available.end(), "genWeight") != available.end();
  if (!presentHlt.empty() && hasGenWeight &&
      std::find(keptBranches.begin(), keptBranches.end(), "genWeight") == keptBranches.end()) {
    keptBranches.push_back("genWeight");
  }

  file.reset();
  ROOT::RDataFrame df(cfg.treeName, inputFile.string());
  auto filtered = presentHlt.empty() ? df.Filter("true") : df.Filter(join(presentHlt, " || "));

  fs::create_directories(scratchFile.parent_path());
  ROOT::RDF::RSnapshotOptions options;
  options.fMode = "RECREATE";
  options.fCompressionLevel = 4;
  filtered.Snapshot(cfg.treeName, scratchFile.string(), keptBranches, options);

  if (!presentHlt.empty() && hasGenWeight) {
    options.fMode = "UPDATE";
    const std::vector<std::string> weightBranch{"genWeight"};
    df.Filter("!(" + join(presentHlt, " || ") + ")")
        .Snapshot(cfg.treeName + "NotPassingHLT", scratchFile.string(), weightBranch, options);
  } else if (!presentHlt.empty()) {
    log.warning(inputFile, " has no genWeight branch; HLT-rejected event weights cannot be retained");
  }
}

struct SkimJob {
  fs::path inputFile;
  fs::path scratchFile;
  fs::path finalFile;
};

void markCompletedJob(const SkimJob &job, bool success, std::vector<std::pair<fs::path, fs::path>> &pending,
                      std::size_t &processed, std::size_t &failed, std::size_t totalFiles, const Config &cfg,
                      Logger &log) {
  ++processed;
  if (success) {
    pending.emplace_back(job.scratchFile, job.finalFile);
  } else {
    ++failed;
    std::error_code ec;
    fs::remove(job.scratchFile, ec);
    log.error("Failed skimming ", job.inputFile);
  }

  if (cfg.progressEveryFiles > 0 && processed % cfg.progressEveryFiles == 0) {
    log.info("Progress: ", processed, "/", totalFiles, " file(s) processed, ", failed, " failed");
  }
  if (pendingBytes(pending) >= cfg.scratchFlushBytes) flushScratch(pending, log);
}

int runSequential(const Config &cfg, const std::vector<SkimJob> &jobs,
                  std::vector<std::pair<fs::path, fs::path>> &pending, Logger &log) {
  std::size_t processed = 0;
  std::size_t failed = 0;
  for (const auto &job : jobs) {
    log.info("Skimming ", job.inputFile, " -> ", job.scratchFile);
    bool success = true;
    try {
      skimOneFile(cfg, job.inputFile, job.scratchFile, log);
    } catch (const std::exception &ex) {
      success = false;
      log.error("Exception while skimming ", job.inputFile, ": ", ex.what());
    }
    markCompletedJob(job, success, pending, processed, failed, jobs.size(), cfg, log);
  }
  return failed == 0 ? 0 : 1;
}

int runMultiprocess(const Config &cfg, const std::vector<SkimJob> &jobs,
                    std::vector<std::pair<fs::path, fs::path>> &pending, Logger &log) {
  std::map<pid_t, SkimJob> active;
  std::size_t nextJob = 0;
  std::size_t processed = 0;
  std::size_t failed = 0;

  auto launchNext = [&]() {
    const auto &job = jobs.at(nextJob++);
    log.info("Skimming ", job.inputFile, " -> ", job.scratchFile);
    const pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed while launching " + job.inputFile.string());
    if (pid == 0) {
      try {
        skimOneFile(cfg, job.inputFile, job.scratchFile, log);
        _exit(0);
      } catch (const std::exception &ex) {
        log.error("Exception while skimming ", job.inputFile, ": ", ex.what());
        _exit(1);
      }
    }
    active.emplace(pid, job);
  };

  while (nextJob < jobs.size() || !active.empty()) {
    while (nextJob < jobs.size() && active.size() < cfg.processes) launchNext();

    int status = 0;
    const pid_t done = waitpid(-1, &status, 0);
    if (done < 0) throw std::runtime_error("waitpid failed");
    auto it = active.find(done);
    if (it == active.end()) continue;
    const bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    markCompletedJob(it->second, success, pending, processed, failed, jobs.size(), cfg, log);
    active.erase(it);
  }

  return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <config.json>\n";
    return 2;
  }

  try {
    const auto cfg = loadConfig(argv[1]);

    const fs::path runOutputDir = cfg.outputBaseDir / runTimestamp();
    fs::create_directories(runOutputDir);
    Logger log(runOutputDir / "skim_nanoaod.log");
    log.info("Starting NanoAOD skim with ", cfg.threads, " thread(s) per process and ", cfg.processes, " process(es)");

    const auto inputDirs = expandInputDirectories(cfg);
    if (inputDirs.empty()) throw std::runtime_error("No input directories matched the configuration");
    const auto rootFiles = collectRootFiles(inputDirs);
    log.info("Found ", rootFiles.size(), " ROOT file(s) under ", inputDirs.size(), " input director(y/ies)");

    const fs::path scratchRunDir = cfg.scratchDir / ("skim_nanoaod_" + runOutputDir.filename().string());
    std::vector<std::pair<fs::path, fs::path>> pending;
    std::vector<SkimJob> jobs;
    jobs.reserve(rootFiles.size());
    for (const auto &inputFile : rootFiles) {
      const auto rel = relativeToBase(inputFile, cfg.inputBaseDir);
      jobs.push_back({inputFile, scratchRunDir / rel, runOutputDir / rel});
    }

    const int status = cfg.processes > 1 ? runMultiprocess(cfg, jobs, pending, log)
                                         : runSequential(cfg, jobs, pending, log);
    flushScratch(pending, log);
    fs::remove_all(scratchRunDir);
    log.info("Finished NanoAOD skim. Output directory: ", runOutputDir);
    return status;
  } catch (const std::exception &ex) {
    std::cerr << "ERROR: " << ex.what() << '\n';
    return 1;
  }

  return 0;
}
