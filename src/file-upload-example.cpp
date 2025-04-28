#include "Particle.h"
#include <vector>
#include <string>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

SYSTEM_MODE(AUTOMATIC);

SerialLogHandler logHandler(LOG_LEVEL_NONE, {{"app", LOG_LEVEL_INFO}});

const char *FILE_DIR = "/upload_test";
const int MIN_FILE_SIZE = 1024;
const int MAX_FILE_SIZE = 15 * 1024; // under the 16KB max event size
const int NUM_FILES = 1;             // number of files to generate

const size_t MAX_EVENT_SIZE = 16384;

struct FileEntry
{
    String path;
    size_t size;
    bool uploaded;
    CloudEvent event;
};
std::vector<FileEntry> files;
String eventName = String::format("file-upload");

void setupFileSystem();
void generateTestFiles();
bool uploadNextFile();
void processCompletedUploads();
void eventStatusChangeCallback(CloudEvent event);
String generateRandomContent(size_t size);

void setup()
{
    Log.info("Starting File Upload Example");

    setupFileSystem();
    generateTestFiles();

    Log.info("File generation complete. Beginning uploads...");
}

void loop()
{
    // Process completed uploads and attempt new ones
    processCompletedUploads();

    // If we have uploads in progress or files to upload, try uploading
    bool allUploaded = true;
    for (const auto &file : files)
    {
        if (!file.uploaded)
        {
            allUploaded = false;
            break;
        }
    }

    if (!allUploaded)
    {
        uploadNextFile();
    }
    else
    {
        // All files uploaded
        static bool completionMessagePrinted = false;
        if (!completionMessagePrinted)
        {
            Log.info("All files uploaded successfully!");
            completionMessagePrinted = true;
        }
    }

    delay(100);
}

void setupFileSystem()
{
    // Create directory if it doesn't exist
    mkdir(FILE_DIR, 0777);

    // Clear any existing files in the directory
    auto testDir = opendir(FILE_DIR);
    if (testDir)
    {
        struct dirent *entry;
        while ((entry = readdir(testDir)) != nullptr)
        {
            if (entry->d_type == DT_REG)
            {
                String filePath = String(FILE_DIR) + "/" + String(entry->d_name);
                remove(filePath);
                Log.info("Removed existing file: %s", filePath.c_str());
            }
        }
        closedir(testDir);
    }
}

void generateTestFiles()
{
    Log.info("Generating %d test files...", NUM_FILES);

    for (int i = 0; i < NUM_FILES; i++)
    {
        // Generate a random file size between MIN_FILE_SIZE and MAX_FILE_SIZE
        size_t fileSize = MIN_FILE_SIZE + (rand() % (MAX_FILE_SIZE - MIN_FILE_SIZE + 1));

        // Create file path
        String filePath = String::format("%s/file_%d.dat", FILE_DIR, i);

        // Create and write random content to the file
        int fd = open(filePath, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd < 0)
        {
            Log.error("Failed to create file: %s", filePath.c_str());
            continue;
        }

        // Generate random content and write to file
        String content = generateRandomContent(fileSize);
        size_t bytesWritten = write(fd, content.c_str(), content.length());
        close(fd);

        if (bytesWritten != content.length())
        {
            Log.error("Failed to write file: %s (wrote %d of %d bytes)",
                      filePath.c_str(), bytesWritten, content.length());
            continue;
        }

        // Add file to tracking list
        FileEntry entry;
        entry.path = filePath;
        entry.size = fileSize;
        entry.uploaded = false;
        files.push_back(entry);

        Log.info("Generated file: %s (%d bytes)", filePath.c_str(), fileSize);
    }

    Log.info("Generated %d files", files.size());
}

bool uploadNextFile()
{
    if (!Particle.connected())
    {
        return false;
    }

    if (!CloudEvent::canPublish(MAX_EVENT_SIZE))
    {
        return false;
    }

    // Find the first file that hasn't been uploaded yet
    for (size_t i = 0; i < files.size(); i++)
    {
        FileEntry &entry = files[i];

        // Skip files that are already uploaded or in progress
        if (entry.uploaded ||
            (entry.event.status() != CloudEvent::Status::NEW &&
             entry.event.status() != CloudEvent::Status::FAILED))
        {
            continue;
        }

        // Open the file and read its contents
        int fd = open(entry.path, O_RDONLY);
        if (fd < 0)
        {
            Log.error("Failed to open file for upload: %s", entry.path.c_str());
            entry.uploaded = true; // Mark as uploaded to skip it
            continue;
        }

        // Read file content
        char *buffer = new char[entry.size];
        if (!buffer)
        {
            Log.error("Failed to allocate memory for file upload");
            close(fd);
            return false;
        }

        size_t bytesRead = read(fd, buffer, entry.size);
        close(fd);

        if (bytesRead != entry.size)
        {
            Log.error("Failed to read file: %s (read %d of %d bytes)",
                      entry.path.c_str(), bytesRead, entry.size);
            delete[] buffer;
            entry.uploaded = true; // Mark as uploaded to skip it
            continue;
        }

        // Setup the CloudEvent
        entry.event = CloudEvent();
        entry.event.name(eventName);
        entry.event.contentType(ContentType::BINARY);
        entry.event.data(buffer, bytesRead);
        entry.event.maxDataInRam(entry.size);
        entry.event.onStatusChange(eventStatusChangeCallback);

        // Attempt to publish
        Log.info("Uploading file: %s (%d bytes) as event: %s",
                 entry.path.c_str(), entry.size, eventName.c_str());

        bool publishResult = Particle.publish(entry.event);

        // Free the buffer
        delete[] buffer;

        if (!publishResult)
        {
            Log.error("Failed to start publish for file: %s", entry.path.c_str());
            return false;
        }

        return true;
    }

    return false;
}

void processCompletedUploads()
{
    for (size_t i = 0; i < files.size(); i++)
    {
        FileEntry &entry = files[i];

        // Check if this file has been uploaded but not yet deleted
        if (!entry.uploaded)
        {
            if (entry.event.status() == CloudEvent::Status::SENT)
            {
                // Upload succeeded, delete the file
                Log.info("Upload successful for file: %s - deleting...", entry.path.c_str());

                // Delete the file
                if (remove(entry.path) == 0)
                {
                    Log.info("Successfully deleted file: %s", entry.path.c_str());
                }
                else
                {
                    Log.error("Failed to delete file: %s", entry.path.c_str());
                }

                // Mark as uploaded
                entry.uploaded = true;
            }
            else if (entry.event.status() == CloudEvent::Status::FAILED)
            {
                // Upload failed, we'll retry in the next loop
                Log.warn("Upload failed for file: %s - will retry", entry.path.c_str());
            }
        }
    }
}

void eventStatusChangeCallback(CloudEvent event)
{
    if (event.status() == CloudEvent::Status::SENT)
    {
        Log.info("Event sent: %s", event.name());
    }
    else if (event.status() == CloudEvent::Status::FAILED)
    {
        Log.error("Event failed: %s", event.name());
    }
}

String generateRandomContent(size_t size)
{
    const char charset[] = "0123456789"
                           "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                           "abcdefghijklmnopqrstuvwxyz";

    String content;
    content.reserve(size);

    for (size_t i = 0; i < size; i++)
    {
        content += charset[rand() % (sizeof(charset) - 1)];
    }

    return content;
}