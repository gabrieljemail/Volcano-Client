#pragma once
#ifndef HELPERS_H
#define HELPERS_H

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>

using namespace std;

vector<char> ReadFile(const string& path)
{
    ifstream file(path, ios::ate | ios::binary);

    if (!file.is_open())
    {
        return {};
    }

    size_t fileSize = file.tellg();
    vector<char> buffer;
    buffer.resize(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}

#endif