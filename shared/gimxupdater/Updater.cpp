/*
 Copyright (c) 2011 Mathieu Laurendeau <mat.lau@laposte.net>
 License: GPLv3
 */

#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <sstream>
#include "updater.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#include <windows.h>
#endif

updater* updater::_singleton = NULL;

#ifdef WIN32
#define CURL_INIT_FLAGS (CURL_GLOBAL_WIN32 | CURL_GLOBAL_SSL)
#else
#define CURL_INIT_FLAGS CURL_GLOBAL_NOTHING
#endif

updater::updater() : client_callback(NULL), client_data(NULL)
{
  //ctor
  curl_global_init(CURL_INIT_FLAGS);
}

updater::~updater()
{
  //dtor
  curl_global_cleanup();
}

static vector<string> &split(const string &s, char delim, vector<string> &elems)
{
  stringstream ss(s);
  string item;
  while (getline(ss, item, delim))
  {
    elems.push_back(item);
  }
  return elems;
}

static vector<string> split(const string &s, char delim)
{
  vector < string > elems;
  return split(s, delim, elems);
}

struct MemoryStruct {
  char *memory;
  size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  mem->memory = (char*) realloc(mem->memory, mem->size + realsize + 1);
  if(mem->memory == NULL) {
    printf("not enough memory (realloc returned NULL)\n");
    return 0;
  }

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

int updater::CheckVersion()
{
  string v;
  int major, minor;
  int old_major, old_minor;
  int ret = -1;

  if(version_url.empty() || version_file.empty() || version.empty())
  {
    return -1;
  }

  CURLcode res;
  struct MemoryStruct chunk;
  chunk.memory = (char*) malloc(sizeof(char));
  chunk.size = 0;

  CURL *curl_handle = curl_easy_init();

  curl_easy_setopt(curl_handle, CURLOPT_URL, VERSION_URL);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
  curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
  res = curl_easy_perform(curl_handle);
  if(res == CURLE_OK)
  {
    string v = string(chunk.memory);

    vector<string> elems = split(v, '.');
    if (elems.size() == 2)
    {
      major = atoi(elems[0].c_str());
      minor = atoi(elems[1].c_str());

      vector<string> old_elems = split(version, '.');
      if (old_elems.size() == 2)
      {
        old_major = atoi(old_elems[0].c_str());
        old_minor = atoi(old_elems[1].c_str());

        ret = (major > old_major || (major == old_major && minor > old_minor));
      }
    }
  }

 /* cleanup curl stuff */
 curl_easy_cleanup(curl_handle);

 if(chunk.memory)
   free(chunk.memory);

 return ret;
}

static size_t write_data(void *ptr, size_t size, size_t nmemb, void *stream)
{
  size_t written = fwrite(ptr, size, nmemb, (FILE *)stream);
  return written;
}

static int progress_callback(void * clientp, double dltotal, double dlnow, double ultotal __attribute__((unused)), double ulnow __attribute__((unused)))
{
    return ((updater *) clientp)->onProgress(dlnow, dltotal);
}

int updater::Update(UPDATER_PROGRESS_CALLBACK callback, void * data, bool wait)
{
  if(download_url.empty())
  {
    return -1;
  }

  string output = "";

#ifdef WIN32
  char temp[MAX_PATH];
  if(!GetTempPathA(sizeof(temp), temp))
  {
    return -1;
  }
  output.append(temp);
#endif
  output.append(download_file);

  FILE* outfile = fopen(output.c_str(), "wb");
  if(outfile)
  {
    CURL *curl_handle = curl_easy_init();

    curl_easy_setopt(curl_handle, CURLOPT_URL, download_url.c_str());
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef WIN32
    curl_easy_setopt(curl_handle, CURLOPT_CAINFO, "ssl/certs/ca-bundle.crt");
#endif
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl_handle, CURLOPT_FILE, outfile);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    if (callback != NULL && data != NULL)
    {
      client_callback = callback;
      client_data = data;
      curl_easy_setopt(curl_handle, CURLOPT_PROGRESSFUNCTION, progress_callback);
      curl_easy_setopt(curl_handle, CURLOPT_PROGRESSDATA, this);
      curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
    }

    //curl_easy_setopt(curl_handle, CURLOPT_VERBOSE, 1L);

    CURLcode res = curl_easy_perform(curl_handle);

    curl_easy_cleanup(curl_handle);

    fclose(outfile);

    if(res != CURLE_OK)
    {
      fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, curl_easy_strerror(res));
      return -1;
    }

#ifdef WIN32

    SHELLEXECUTEINFO shExInfo = SHELLEXECUTEINFO();
    shExInfo.cbSize = sizeof(shExInfo);
    shExInfo.fMask = wait ? (SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC) : SEE_MASK_DEFAULT;
    shExInfo.hwnd = 0;
    shExInfo.lpVerb = "runas";
    shExInfo.lpFile = output.c_str();
    shExInfo.lpParameters = "";
    shExInfo.lpDirectory = 0;
    shExInfo.nShow = SW_SHOW;
    shExInfo.hInstApp = 0;

    if (!ShellExecuteEx(&shExInfo))
    {
        return -1;
    }

    WaitForSingleObject(shExInfo.hProcess, INFINITE);
    CloseHandle(shExInfo.hProcess);

#else
    string cmd = "";
    cmd.append("xdg-open ");
    cmd.append(download_file);
    if (!wait)
    {
      cmd.append("&");
    }

    if(system(cmd.c_str()))
    {
      return -1;
    }
#endif
  }

  return 0;
}
