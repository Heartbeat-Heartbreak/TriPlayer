#ifndef UTILS_FS_HPP
#define UTILS_FS_HPP

#include <string>
#include <vector>

// Helper functions for interacting with the filesystem
namespace Utils::Fs {
    // Create directories forming path
    // Returns false on an error
    bool createPath(const std::string &);

    // Copy file from src to dest
    // Returns true if successful, false otherwise
    bool copyFile(const std::string &, const std::string &);

    // All return true based on condition
    bool fileAccessible(const std::string &);
    bool fileExists(const std::string &);

    // Returns contents of directory
    // First element in pair is path, second is set true if directory
    std::vector< std::pair<std::string, bool> > getDirectoryContents(const std::string &);
    // Returns extension of path (empty if not a file or no extension)
    std::string getExtension(const std::string &);
    // Returns stem of path
    std::string getStem(const std::string &);
    // Returns parent directory
    std::string getParentDirectory(const std::string &);

    // Append data to a file
    bool appendFile(const std::string &, const std::vector<unsigned char> &);
    // Delete a file
    void deleteFile(const std::string &);
    // Read an entire file into the buffer
    bool readFile(const std::string &, std::vector<unsigned char> &);
    // Write entire contents of buffer to file
    bool writeFile(const std::string &, const std::vector<unsigned char> &);
};

#endif