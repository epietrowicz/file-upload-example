# Particle File Upload Example
This project shows how to use Particle's extended publish feature (available in 6.3.0+) to upload a number of files from the device's file system.

On boot, a specific number of files will be generated and filled with random values via `generateTestFiles`. These files serve as an example and would likely be the result of some other process. The files are then queued in `std::vector<FileEntry> files` and serviced in the `main` loop by the non-blocking `uploadNextFile` function. The files are deleted upon successful upload via `processCompletedUploads`.