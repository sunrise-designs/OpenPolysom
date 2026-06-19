There are two main components to this project:
1. Hardware data acquisition. It runs on a bespoke PCB board with ESP32-S3 microcontroller (WIFI/BLE/512KB SRAM, 384KB ROM, 8MB PSRAM, 8MB Flash memory)
2. Mixed local and cloud-based signal processing and visualization

I need help to decide on the architecture and data flow of the component 2, the data pipeline once it leaves the device.

The plan so far is:
1. ESP32 saves the various sampled signals (heart rate, sound, breathing, movement, EEG, etc) to an SD card. A simple proprietary binary format, which minimizes SD Card FTL Thrashing, will be used. The embedded code will be in C++
2. A python codebase ingests the binary data, and saves it to an intermediate format: Zarr binary file for high-density data, and a sidecar .json file for sparse metadata (like patient data, time of the experiment, settings and so on). Minimum processing happens at this stage.
3. In a second step, a signal processing Python codebase performs signal analysis on the data. This could become quite complex, incorporating elaborate DSP, and possibly ML and AI components. This will be the heart of the Polysom, it must be tightly version controlled, unit tested, and auditable. It will save the processed results to a new Zarr file (so that the raw data is preserved for auditability) and the results metadata .json file.
4. A Typescript codebase generates a web page which displays the charts via Apache ECharts, using HTTP Range Requests to enable responsive viewing of large datasets, and the associated metadata from the sidecar .json file(s)


Questions:
1. Does this sound sane?
2. Is ECharts the best option? Is there anything better/more performant/easier to use? Code Maintainability is very important.
2. Report generation in .pdf file format is very important. The current trade-off is to save the charts in a raster format, and combine them into a structured .pdf file where the metadata is selectable, bookmarked, and search-able. What are other potential options for this, if better ones exit?
3. This needs to be cross-platform as much as possible
3. Ideally, this should run locally, as well as on the web, in an air-gapped environment. Can the required infrastructure for this data flow (Python + packages, npm, etc) be realistically packaged into Windows, Linux and Mac installers?
