#include "CurlMarketplaceClient.hpp"
#include <curl/curl.h>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <iostream>

namespace rex::modules {

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    try {
        s->append((char*)contents, newLength);
        return newLength;
    } catch (std::bad_alloc&) {
        return 0;
    }
}

static size_t WriteFileCallback(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

CurlMarketplaceClient::CurlMarketplaceClient(const std::string& baseUrl) : baseUrl_(baseUrl) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

std::string CurlMarketplaceClient::httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to init curl");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error("Curl GET failed: " + std::string(curl_easy_strerror(res)));
    }
    return response;
}

bool CurlMarketplaceClient::httpDownload(const std::string& url, const std::string& filePath) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FILE* fp = fopen(filePath.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        std::cerr << "Download failed: " << curl_easy_strerror(res) << "\n";
        return false;
    }
    return true;
}

std::vector<PluginManifest> CurlMarketplaceClient::fetchAvailablePlugins() {
    std::string jsonStr = httpGet(baseUrl_ + "/api/v1/plugins");
    std::vector<PluginManifest> result;
    return result;
}

bool CurlMarketplaceClient::downloadPlugin(const std::string& url, const std::string& destinationPath) {
    return httpDownload(url, destinationPath);
}

std::string CurlMarketplaceClient::downloadSignature(const std::string& url) {
    return httpGet(url);
}

} // namespace rex::modules
