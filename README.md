# SkimNanoAOD

C++17/ROOT `RDataFrame::Snapshot` based NanoAOD skim utility.

## Features

- Reads a JSON configuration file to control input directories, branch keep list, HLT OR filters, process/thread parallelism, scratch flushing, and progress logging.
- Recursively finds `.root` files below one or more configured input directories. Wildcards are supported in each input directory entry.
- Preserves the input directory structure below `input_base_directory` when writing to `output_base_directory/<YYYYMMDD_HHMMSS>/`.
- Writes skimmed ROOT files to a scratch run directory first, then flushes them to the final output directory when the configured scratch byte threshold is reached.
- Logs execution details and missing configured branches/HLT paths to `skim_nanoaod.log` in the timestamped output directory.
- Ignores missing keep-list branches and missing HLT paths with `WARNING` messages, then snapshots only the branches that exist in the input file.

## Build

Dependencies:

- ROOT with `RIO`, `Tree`, and `ROOTDataFrame`
- `nlohmann_json`
- CMake 3.16+

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

Start from the template and edit it for your dataset:

```bash
cp config/skim_config_template.json skim_config.json
./build/skim_nanoaod skim_config.json
```

For an input file such as:

```text
/data2/common/NanoAOD/mc/v15/RunIII2024Summer24NanoAODv15/TTtoLNu2Q_TuneCP5_13p6TeV_powheg-pythia8/NANOAODSIM/150X_mcRun3_2024_realistic_v2-v2/2520000/12e623dc-f390-4ff0-8fe3-81379519adce.root
```

and a run timestamp of `20260702_130454`, the output path is:

```text
/data2/common/skimmed_NanoAOD/20260702_130454/mc/v15/RunIII2024Summer24NanoAODv15/TTtoLNu2Q_TuneCP5_13p6TeV_powheg-pythia8/NANOAODSIM/150X_mcRun3_2024_realistic_v2-v2/2520000/12e623dc-f390-4ff0-8fe3-81379519adce.root
```

## JSON configuration

See [`config/skim_config_template.json`](config/skim_config_template.json).

Important keys:

- `threads`: number of ROOT implicit multi-threading threads per process.
- `processes`: number of parallel worker processes. Total CPU concurrency is roughly `threads * processes`, so tune both values for the host and storage system.
- `input_base_directory`: base NanoAOD directory, default `/data2/common/NanoAOD/`.
- `output_base_directory`: base skim output directory, default `/data2/common/skimmed_NanoAOD/`.
- `scratch_directory`: temporary directory used before final flush, default `/scratch`.
- `scratch_flush_bytes`: flush threshold for pending scratch files.
- `input_directories`: array of directories or wildcard patterns relative to `input_base_directory`; all subdirectories are searched.
- `branches`: branch keep list. Shell-style wildcards such as `Muon_*` and `Jet_btag*` are allowed. Patterns that match no branch are logged as warnings and skipped.
- `hlt_paths`: HLT branches combined with OR for event filtering. Shell-style wildcards such as `HLT_Mu*` are allowed. Patterns that match no HLT branch are logged as warnings and skipped.
- `progress_every_files`: progress log frequency in processed files.
- `tree_name`: tree to skim, normally `Events` for NanoAOD.
