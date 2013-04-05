// -*- coding: utf-8 -*-
// Copyright (C) 2012-2013 MUJIN Inc. <rosen.diankov@mujin.co.jp>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "common.h"
#include "controllerclientimpl.h"

#define SKIP_PEER_VERIFICATION // temporary
//#define SKIP_HOSTNAME_VERIFICATION

namespace mujinclient {

class CurlCustomRequestSetter
{
public:
    CurlCustomRequestSetter(CURL *curl, const char* method) : _curl(curl) {
        curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, method);
    }
    ~CurlCustomRequestSetter() {
        curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, NULL);
    }
protected:
    CURL* _curl;
};

class CurlUploadSetter
{
public:
    CurlUploadSetter(CURL *curl) : _curl(curl) {
        curl_easy_setopt(_curl, CURLOPT_UPLOAD, 1L);
    }
    ~CurlUploadSetter() {
        curl_easy_setopt(_curl, CURLOPT_UPLOAD, 0L);
    }
protected:
    CURL* _curl;
};

template <typename T>
std::wstring ParseWincapsWCNPath(const T& sourcefilename, const boost::function<std::string(const T&)>& ConvertToFileSystemEncoding)
{
    // scenefilenames is the WPJ file, so have to open it up to see what directory it points to
    // note that the encoding is utf-16
    // <clsProject Object="True">
    //   <WCNPath VT="8">.\threegoaltouch\threegoaltouch.WCN;</WCNPath>
    // </clsProject>
    // first have to get the raw utf-16 data
#if defined(_WIN32) || defined(_WIN64)
    std::ifstream wpjfilestream(sourcefilename.c_str(), std::ios::binary|std::ios::in);
#else
    // linux doesn't mix ifstream and wstring
    std::ifstream wpjfilestream(ConvertToFileSystemEncoding(sourcefilename).c_str(), std::ios::binary|std::ios::in);
#endif
    if( !wpjfilestream ) {
        throw MUJIN_EXCEPTION_FORMAT("failed to open file %s", ConvertToFileSystemEncoding(sourcefilename), MEC_InvalidArguments);
    }
    std::wstringstream utf16stream;
    bool readbom = false;
    while(!wpjfilestream.eof() ) {
        unsigned short c;
        wpjfilestream.read(reinterpret_cast<char*>(&c),sizeof(c));
        if( !wpjfilestream ) {
            break;
        }
        // skip the first character (BOM) due to a bug in boost property_tree (should be fixed in 1.49)
        if( readbom || c != 0xfeff ) {
            utf16stream << static_cast<wchar_t>(c);
        }
        else {
            readbom = true;
        }
    }
    boost::property_tree::wptree wpj;
    boost::property_tree::read_xml(utf16stream, wpj);
    boost::property_tree::wptree& clsProject = wpj.get_child(L"clsProject");
    boost::property_tree::wptree& WCNPath = clsProject.get_child(L"WCNPath");
    std::wstring strWCNPath = WCNPath.data();
    if( strWCNPath.size() > 0 ) {
        // post process the string to get the real filesystem directory
        if( strWCNPath.at(strWCNPath.size()-1) == L';') {
            strWCNPath.resize(strWCNPath.size()-1);
        }

        if( strWCNPath.size() >= 2 && (strWCNPath[0] == L'.' && strWCNPath[1] == L'\\') ) {
            // don't need the prefix
            strWCNPath = strWCNPath.substr(2);
        }
    }

    return strWCNPath;
}

ControllerClientImpl::ControllerClientImpl(const std::string& usernamepassword, const std::string& baseuri, const std::string& proxyserverport, const std::string& proxyuserpw, int options)
{
    size_t usernameindex = usernamepassword.find_first_of(':');
    BOOST_ASSERT(usernameindex != std::string::npos );
    std::string username = usernamepassword.substr(0,usernameindex);
    std::string password = usernamepassword.substr(usernameindex+1);

    _httpheaders = NULL;
    if( baseuri.size() > 0 ) {
        _baseuri = baseuri;
        // ensure trailing slash
        if( _baseuri[_baseuri.size()-1] != '/' ) {
            _baseuri.push_back('/');
        }
    }
    else {
        // use the default
        _baseuri = "https://controller.mujin.co.jp/";
    }
    _baseapiuri = _baseuri + std::string("api/v1/");
    // hack for now since webdav server and api server could be running on different ports
    if( boost::algorithm::ends_with(_baseuri, ":8000/") ) {
        // testing on localhost, however the webdav server is running on port 80...
        _basewebdavuri = str(boost::format("%s/u/%s/")%_baseuri.substr(0,_baseuri.size()-6)%username);
    }
    else {
        _basewebdavuri = str(boost::format("%su/%s/")%_baseuri%username);
    }

    //CURLcode code = curl_global_init(CURL_GLOBAL_SSL|CURL_GLOBAL_WIN32);
    _curl = curl_easy_init();
    BOOST_ASSERT(!!_curl);

#ifdef _DEBUG
    //curl_easy_setopt(_curl, CURLOPT_VERBOSE, 1L);
#endif
    _errormessage.resize(CURL_ERROR_SIZE);
    curl_easy_setopt(_curl, CURLOPT_ERRORBUFFER, &_errormessage[0]);

    CURLcode res;
#ifdef SKIP_PEER_VERIFICATION
    /*
     * if you want to connect to a site who isn't using a certificate that is
     * signed by one of the certs in the ca bundle you have, you can skip the
     * verification of the server's certificate. this makes the connection
     * a lot less secure.
     *
     * if you have a ca cert for the server stored someplace else than in the
     * default bundle, then the curlopt_capath option might come handy for
     * you.
     */
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYPEER, 0l);
#endif

#ifdef SKIP_HOSTNAME_VERIFICATION
    /*
     * If the site you're connecting to uses a different host name that what
     * they have mentioned in their server certificate's commonName (or
     * subjectAltName) fields, libcurl will refuse to connect. You can skip
     * this check, but this will make the connection less secure.
     */
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif

    if( proxyserverport.size() > 0 ) {
        SetProxy(proxyserverport, proxyuserpw);
    }

    res = curl_easy_setopt(_curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    CHECKCURLCODE(res, "failed to set auth");
    res = curl_easy_setopt(_curl, CURLOPT_USERPWD, usernamepassword.c_str());
    CHECKCURLCODE(res, "failed to set userpw");

    // need to set the following?
    //CURLOPT_USERAGENT
    //CURLOPT_TCP_KEEPIDLE
    //CURLOPT_TCP_KEEPALIVE
    //CURLOPT_TCP_KEEPINTVL

    curl_easy_setopt(_curl, CURLOPT_COOKIEFILE, ""); // just to start the cookie engine

    // save everything to _buffer, neceesary to do it before first POST/GET calls or data will be output to stdout
    res = curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, _writer);
    CHECKCURLCODE(res, "failed to set writer");
    res = curl_easy_setopt(_curl, CURLOPT_WRITEDATA, &_buffer);
    CHECKCURLCODE(res, "failed to set write data");

    std::string useragent = std::string("controllerclientcpp/")+MUJINCLIENT_VERSION_STRING;
    res = curl_easy_setopt(_curl, CURLOPT_USERAGENT, useragent.c_str());
    CHECKCURLCODE(res, "failed to set user-agent");

    res = curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, 0); // do not bounce through pages since we need to detect when login sessions expired
    CHECKCURLCODE(res, "failed to set follow location");
    res = curl_easy_setopt(_curl, CURLOPT_MAXREDIRS, 10);
    CHECKCURLCODE(res, "failed to max redirs");

    if( !(options & 1) ) {
        // make an initial GET call to get the CSRF token
        std::string loginuri = _baseuri + "login/";
        curl_easy_setopt(_curl, CURLOPT_URL, loginuri.c_str());
        curl_easy_setopt(_curl, CURLOPT_HTTPGET, 1);
        CURLcode res = curl_easy_perform(_curl);
        CHECKCURLCODE(res, "curl_easy_perform failed");
        long http_code = 0;
        res=curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
        CHECKCURLCODE(res, "curl_easy_getinfo");
        if( http_code == 302 ) {
            // most likely apache2-only authentication and login page isn't needed, however need to send another GET for the csrftoken
            loginuri = _baseuri + "api/v1/"; // pick some neutral page that is easy to load
            curl_easy_setopt(_curl, CURLOPT_URL, loginuri.c_str());
            CURLcode res = curl_easy_perform(_curl);
            CHECKCURLCODE(res, "curl_easy_perform failed");
            long http_code = 0;
            res = curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
            CHECKCURLCODE(res, "curl_easy_getinfo");
            if( http_code != 200 ) {
                throw MUJIN_EXCEPTION_FORMAT("HTTP GET %s returned HTTP error code %s", loginuri%http_code, MEC_HTTPServer);
            }
            _csrfmiddlewaretoken = _GetCSRFFromCookies();
            curl_easy_setopt(_curl, CURLOPT_REFERER, loginuri.c_str()); // necessary for SSL to work
        }
        else if( http_code == 200 ) {
            _csrfmiddlewaretoken = _GetCSRFFromCookies();
            std::string data = str(boost::format("username=%s&password=%s&this_is_the_login_form=1&next=%%2F&csrfmiddlewaretoken=%s")%username%password%_csrfmiddlewaretoken);
            curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, data.size());
            curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, data.c_str());
            curl_easy_setopt(_curl, CURLOPT_REFERER, loginuri.c_str());
            //std::cout << "---performing post---" << std::endl;
            res = curl_easy_perform(_curl);
            CHECKCURLCODE(res, "curl_easy_perform failed");
            http_code = 0;
            res = curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
            CHECKCURLCODE(res, "curl_easy_getinfo failed");
            if( http_code != 200 && http_code != 302 ) {
                throw MUJIN_EXCEPTION_FORMAT("User login failed. HTTP POST %s returned HTTP status %s", loginuri%http_code, MEC_UserAuthentication);
            }
        }
        else {
            throw MUJIN_EXCEPTION_FORMAT("HTTP GET %s returned HTTP error code %s", loginuri%http_code, MEC_HTTPServer);
        }
    }

    _charset = "utf-8";
    _language = "en-us";
#if defined(_WIN32) || defined(_WIN64)
    UINT codepage = GetACP();
    std::map<int, std::string>::const_iterator itcodepage = encoding::GetCodePageMap().find(codepage);
    if( itcodepage != encoding::GetCodePageMap().end() ) {
        _charset = itcodepage->second;
    }
#endif
    std::cout << "setting character set to " << _charset << std::endl;
    _SetHTTPHeaders();

    try {
        GetProfile();
    }
    catch(const MujinException&) {
        // most likely username or password are
        throw MujinException(str(boost::format("failed to get controller profile, check username/password or if controller is active at %s")%_baseuri), MEC_UserAuthentication);
    }
}

ControllerClientImpl::~ControllerClientImpl()
{
    if( !!_httpheaders ) {
        curl_slist_free_all(_httpheaders);
    }
    curl_easy_cleanup(_curl);
}

std::string ControllerClientImpl::GetVersion()
{
    return _profile.get<std::string>("version");
}


void ControllerClientImpl::SetCharacterEncoding(const std::string& newencoding)
{
    boost::mutex::scoped_lock lock(_mutex);
    _charset = newencoding;
    _SetHTTPHeaders();
}

void ControllerClientImpl::SetProxy(const std::string& serverport, const std::string& userpw)
{
    curl_easy_setopt(_curl, CURLOPT_PROXY, serverport.c_str());
    curl_easy_setopt(_curl, CURLOPT_PROXYUSERPWD, userpw.c_str());
}

void ControllerClientImpl::SetLanguage(const std::string& language)
{
    boost::mutex::scoped_lock lock(_mutex);
    _language = language;
    _SetHTTPHeaders();
}

void ControllerClientImpl::RestartServer()
{
    boost::mutex::scoped_lock lock(_mutex);
    _uri = _baseuri + std::string("ajax/restartserver/");
    curl_easy_setopt(_curl, CURLOPT_URL, _uri.c_str());
    curl_easy_setopt(_curl, CURLOPT_POST, 1);
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, 0);
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, NULL);
    CURLcode res = curl_easy_perform(_curl);
    CHECKCURLCODE(res, "curl_easy_perform failed");
    long http_code = 0;
    res = curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
    CHECKCURLCODE(res, "curl_easy_getinfo failed");
    if( http_code != 200 ) {
        throw MUJIN_EXCEPTION_FORMAT0("Failed to restart server, please try again or contact MUJIN support", MEC_HTTPServer);
    }
}

void ControllerClientImpl::Upgrade(const std::vector<unsigned char>& vdata)
{
    BOOST_ASSERT(vdata.size()>0);
    boost::mutex::scoped_lock lock(_mutex);
    _uri = _baseuri + std::string("upgrade/");
    curl_easy_setopt(_curl, CURLOPT_URL, _uri.c_str());
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, NULL);
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, NULL);

    // set new headers and remove the Expect: 100-continue
    struct curl_slist *headerlist=NULL;
    headerlist = curl_slist_append(headerlist, "Expect:");
    std::string s = std::string("X-CSRFToken: ")+_csrfmiddlewaretoken;
    headerlist = curl_slist_append(headerlist, s.c_str());
    curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, headerlist);

    // Fill in the file upload field
    struct curl_httppost *formpost=NULL, *lastptr=NULL;
    curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "file", CURLFORM_BUFFER, "mujinpatch", CURLFORM_BUFFERPTR, &vdata[0], CURLFORM_BUFFERLENGTH, vdata.size(), CURLFORM_END);
    curl_easy_setopt(_curl, CURLOPT_HTTPPOST, formpost);
    CURLcode res = curl_easy_perform(_curl);
    curl_formfree(formpost);
    // reset the headers before any exceptions are thrown
    _SetHTTPHeaders();
    CHECKCURLCODE(res, "curl_easy_perform failed");
    long http_code = 0;
    res = curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
    CHECKCURLCODE(res, "curl_easy_getinfo failed");
    if( http_code != 200 ) {
        throw MUJIN_EXCEPTION_FORMAT0("Failed to upgrade server, please try again or contact MUJIN support", MEC_HTTPServer);
    }
}

void ControllerClientImpl::CancelAllJobs()
{
    CallDelete("job/?format=json");
}

void ControllerClientImpl::GetRunTimeStatuses(std::vector<JobStatus>& statuses, int options)
{
    boost::property_tree::ptree pt;
    std::string url = "job/?format=json&fields=pk,status,fnname,elapsedtime";
    if( options & 1 ) {
        url += std::string(",status_text");
    }
    CallGet(url, pt);
    boost::property_tree::ptree& objects = pt.get_child("objects");
    size_t i = 0;
    statuses.resize(objects.size());
    BOOST_FOREACH(boost::property_tree::ptree::value_type &v, objects) {
        statuses[i].pk = v.second.get<std::string>("pk");
        statuses[i].code = static_cast<JobStatusCode>(boost::lexical_cast<int>(v.second.get<std::string>("status")));
        statuses[i].type = v.second.get<std::string>("fnname");
        statuses[i].elapsedtime = v.second.get<double>("elapsedtime");
        if( options & 1 ) {
            statuses[i].message = v.second.get<std::string>("status_text");
        }
        i++;
    }
}

void ControllerClientImpl::GetScenePrimaryKeys(std::vector<std::string>& scenekeys)
{
    boost::property_tree::ptree pt;
    CallGet("scene/?format=json&limit=0&fields=pk", pt);
    boost::property_tree::ptree& objects = pt.get_child("objects");
    scenekeys.resize(objects.size());
    size_t i = 0;
    BOOST_FOREACH(boost::property_tree::ptree::value_type &v, objects) {
        scenekeys[i++] = v.second.get<std::string>("pk");
    }
}

SceneResourcePtr ControllerClientImpl::RegisterScene(const std::string& uri, const std::string& scenetype)
{
    BOOST_ASSERT(scenetype.size()>0);
    boost::property_tree::ptree pt;
    CallPost("scene/?format=json&fields=pk", str(boost::format("{\"uri\":\"%s\", \"scenetype\":\"%s\"}")%uri%scenetype), pt);
    std::string pk = pt.get<std::string>("pk");
    SceneResourcePtr scene(new SceneResource(shared_from_this(), pk));
    return scene;
}

SceneResourcePtr ControllerClientImpl::ImportSceneToCOLLADA(const std::string& importuri, const std::string& importformat, const std::string& newuri)
{
    BOOST_ASSERT(importformat.size()>0);
    boost::property_tree::ptree pt;
    CallPost("scene/?format=json&fields=pk", str(boost::format("{\"reference_uri\":\"%s\", \"reference_format\":\"%s\", \"uri\":\"%s\"}")%importuri%importformat%newuri), pt);
    std::string pk = pt.get<std::string>("pk");
    SceneResourcePtr scene(new SceneResource(shared_from_this(), pk));
    return scene;
}

void ControllerClientImpl::SyncUpload_UTF8(const std::string& sourcefilename, const std::string& destinationdir, const std::string& scenetype)
{
    // TODO use curl_multi_perform to allow uploading of multiple files simultaneously
    // TODO should LOCK with WebDAV repository?
    boost::mutex::scoped_lock lock(_mutex);
    std::string baseuploaduri;
    if( destinationdir.size() >= 7 && destinationdir.substr(0,7) == "mujin:/" ) {
        baseuploaduri = _basewebdavuri;
        baseuploaduri += _EncodeWithoutSeparator(destinationdir.substr(7));
        _EnsureWebDAVDirectories(destinationdir.substr(7));
    }
    else {
        baseuploaduri = destinationdir;
    }
    // ensure trailing slash
    if( baseuploaduri[baseuploaduri.size()-1] != '/' ) {
        baseuploaduri.push_back('/');
    }

    size_t nBaseFilenameStartIndex = sourcefilename.find_last_of(s_filesep);
    if( nBaseFilenameStartIndex == std::string::npos ) {
        // there's no path?
        nBaseFilenameStartIndex = 0;
    }
    else {
        nBaseFilenameStartIndex++;
    }

    if( scenetype == "wincaps" ) {
        std::wstring strWCNPath_utf16 = ParseWincapsWCNPath<std::string>(sourcefilename, encoding::ConvertUTF8ToFileSystemEncoding);
        if( strWCNPath_utf16.size() > 0 ) {
            std::string strWCNPath;
            utf8::utf16to8(strWCNPath_utf16.begin(), strWCNPath_utf16.end(), std::back_inserter(strWCNPath));
            std::string strWCNURI = strWCNPath;
            size_t lastindex = 0;
            for(size_t i = 0; i < strWCNURI.size(); ++i) {
                if( strWCNURI[i] == '\\' ) {
                    strWCNURI[i] = '/';
                    strWCNPath[i] = s_filesep;
                    lastindex = i;
                }
            }
            std::string sCopyDir = sourcefilename.substr(0,nBaseFilenameStartIndex) + strWCNPath.substr(0,lastindex);
            _UploadDirectoryToWebDAV_UTF8(sCopyDir, baseuploaduri+_EncodeWithoutSeparator(strWCNURI.substr(0,lastindex)));
        }
    }

    // sourcefilenamebase is utf-8
    char* pescapeddir = curl_easy_escape(_curl, sourcefilename.substr(nBaseFilenameStartIndex).c_str(), 0);
    std::string uploadfileuri = baseuploaduri + std::string(pescapeddir);
    curl_free(pescapeddir);
    _UploadFileToWebDAV_UTF8(sourcefilename, uploadfileuri);

    /* webdav operations
       const char *postdata =
       "<?xml version=\"1.0\"?><D:searchrequest xmlns:D=\"DAV:\" >"
       "<D:sql>SELECT \"http://schemas.microsoft.com/repl/contenttag\""
       " from SCOPE ('deep traversal of \"/exchange/adb/Calendar/\"') "
       "WHERE \"DAV:isfolder\" = True</D:sql></D:searchrequest>\r\n";
     */
}

void ControllerClientImpl::SyncUpload_UTF16(const std::wstring& sourcefilename_utf16, const std::wstring& destinationdir_utf16, const std::string& scenetype)
{
    // TODO use curl_multi_perform to allow uploading of multiple files simultaneously
    // TODO should LOCK with WebDAV repository?
    boost::mutex::scoped_lock lock(_mutex);
    std::string baseuploaduri;
    std::string destinationdir_utf8;
    utf8::utf16to8(destinationdir_utf16.begin(), destinationdir_utf16.end(), std::back_inserter(destinationdir_utf8));

    if( destinationdir_utf8.size() >= 7 && destinationdir_utf8.substr(0,7) == "mujin:/" ) {
        baseuploaduri = _basewebdavuri;
        std::string s = destinationdir_utf8.substr(7);
        baseuploaduri += _EncodeWithoutSeparator(s);
        _EnsureWebDAVDirectories(s);
    }
    else {
        baseuploaduri = destinationdir_utf8;
    }
    // ensure trailing slash
    if( baseuploaduri[baseuploaduri.size()-1] != '/' ) {
        baseuploaduri.push_back('/');
    }

    size_t nBaseFilenameStartIndex = sourcefilename_utf16.find_last_of(s_wfilesep);
    if( nBaseFilenameStartIndex == std::string::npos ) {
        // there's no path?
        nBaseFilenameStartIndex = 0;
    }
    else {
        nBaseFilenameStartIndex++;
    }

    if( scenetype == "wincaps" ) {
        std::wstring strWCNPath_utf16 = ParseWincapsWCNPath<std::wstring>(sourcefilename_utf16, encoding::ConvertUTF16ToFileSystemEncoding);
        if( strWCNPath_utf16.size() > 0 ) {
            std::string strWCNURI;
            utf8::utf16to8(strWCNPath_utf16.begin(), strWCNPath_utf16.end(), std::back_inserter(strWCNURI));
            size_t lastindex_utf8 = 0;
            for(size_t i = 0; i < strWCNURI.size(); ++i) {
                if( strWCNURI[i] == '\\' ) {
                    strWCNURI[i] = '/';
                    lastindex_utf8 = i;
                }
            }
            size_t lastindex_utf16 = 0;
            for(size_t i = 0; i < strWCNPath_utf16.size(); ++i) {
                if( strWCNPath_utf16[i] == '\\' ) {
                    strWCNPath_utf16[i] = s_wfilesep;
                    lastindex_utf16 = i;
                }
            }
            std::wstring sCopyDir_utf16 = sourcefilename_utf16.substr(0,nBaseFilenameStartIndex) + strWCNPath_utf16.substr(0,lastindex_utf16);
            _UploadDirectoryToWebDAV_UTF16(sCopyDir_utf16, baseuploaduri+_EncodeWithoutSeparator(strWCNURI.substr(0,lastindex_utf8)));
        }
    }

    // sourcefilenamebase is utf-8
    std::string sourcefilenamedir_utf8;
    utf8::utf16to8(sourcefilename_utf16.begin()+nBaseFilenameStartIndex, sourcefilename_utf16.end(), std::back_inserter(sourcefilenamedir_utf8));
    char* pescapeddir = curl_easy_escape(_curl, sourcefilenamedir_utf8.c_str(), 0);
    std::string uploadfileuri = baseuploaduri + std::string(pescapeddir);
    curl_free(pescapeddir);
    _UploadFileToWebDAV_UTF16(sourcefilename_utf16, uploadfileuri);
}

/// \brief expectedhttpcode is not 0, then will check with the returned http code and if not equal will throw an exception
int ControllerClientImpl::CallGet(const std::string& relativeuri, boost::property_tree::ptree& pt, int expectedhttpcode)
{
    boost::mutex::scoped_lock lock(_mutex);
    _uri = _baseapiuri;
    _uri += relativeuri;
    curl_easy_setopt(_curl, CURLOPT_URL, _uri.c_str());
    _buffer.clear();
    _buffer.str("");
    curl_easy_setopt(_curl, CURLOPT_HTTPGET, 1);
    CURLcode res = curl_easy_perform(_curl);
    CHECKCURLCODE(res, "curl_easy_perform failed");
    long http_code = 0;
    res=curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
    CHECKCURLCODE(res, "curl_easy_getinfo");
    if( _buffer.rdbuf()->in_avail() > 0 ) {
        boost::property_tree::read_json(_buffer, pt);
    }
    if( expectedhttpcode != 0 && http_code != expectedhttpcode ) {
        std::string error_message = pt.get<std::string>("error_message", std::string());
        std::string traceback = pt.get<std::string>("traceback", std::string());
        throw MUJIN_EXCEPTION_FORMAT("HTTP GET to '%s' returned HTTP status %s: %s", relativeuri%http_code%error_message, MEC_HTTPServer);
    }
    return http_code;
}

/// \brief expectedhttpcode is not 0, then will check with the returned http code and if not equal will throw an exception
int ControllerClientImpl::CallGet(const std::string& relativeuri, std::string& outputdata, int expectedhttpcode)
{
    boost::mutex::scoped_lock lock(_mutex);
    _uri = _baseapiuri;
    _uri += relativeuri;
    curl_easy_setopt(_curl, CURLOPT_URL, _uri.c_str());
    _buffer.clear();
    _buffer.str("");
    curl_easy_setopt(_curl, CURLOPT_HTTPGET, 1);
    CURLcode res = curl_easy_perform(_curl);
    CHECKCURLCODE(res, "curl_easy_perform failed");
    long http_code = 0;
    res=curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
    CHECKCURLCODE(res, "curl_easy_getinfo");
    outputdata = _buffer.str();
    if( expectedhttpcode != 0 && http_code != expectedhttpcode ) {
        if( outputdata.size() > 0 ) {
            boost::property_tree::ptree pt;
            boost::property_tree::read_json(_buffer, pt);
            std::string error_message = pt.get<std::string>("error_message", std::string());
            std::string traceback = pt.get<std::string>("traceback", std::string());
            throw MUJIN_EXCEPTION_FORMAT("HTTP GET to '%s' returned HTTP status %s: %s", relativeuri%http_code%error_message, MEC_HTTPServer);
        }
        throw MUJIN_EXCEPTION_FORMAT("HTTP GET to '%s' returned HTTP status %s", relativeuri%http_code, MEC_HTTPServer);
    }
    return http_code;
}

/// \brief expectedhttpcode is not 0, then will check with the returned http code and if not equal will throw an exception
int ControllerClientImpl::CallPost(const std::string& relativeuri, const std::string& data, boost::property_tree::ptree& pt, int expectedhttpcode)
{
    boost::mutex::scoped_lock lock(_mutex);
    _uri = _baseapiuri;
    _uri += relativeuri;
    curl_easy_setopt(_curl, CURLOPT_URL, _uri.c_str());
    _buffer.clear();
    _buffer.str("");
    curl_easy_setopt(_curl, CURLOPT_POST, 1);
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, data.size());
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, data.size() > 0 ? data.c_str() : NULL);
    CURLcode res = curl_easy_perform(_curl);
    CHECKCURLCODE(res, "curl_easy_perform failed");
    long http_code = 0;
    res = curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
    CHECKCURLCODE(res, "curl_easy_getinfo failed");
    if( _buffer.rdbuf()->in_avail() > 0 ) {
        boost::property_tree::read_json(_buffer, pt);
    }
    if( expectedhttpcode != 0 && http_code != expectedhttpcode ) {
        std::string error_message = pt.get<std::string>("error_message", std::string());
        std::string traceback = pt.get<std::string>("traceback", std::string());
        throw MUJIN_EXCEPTION_FORMAT("HTTP POST to '%s' returned HTTP status %s: %s", relativeuri%http_code%error_message, MEC_HTTPServer);
    }
    return http_code;
}

int ControllerClientImpl::CallPut(const std::string& relativeuri, const std::string& data, boost::property_tree::ptree& pt, int expectedhttpcode)
{
    boost::mutex::scoped_lock lock(_mutex);
    _uri = _baseapiuri;
    _uri += relativeuri;
    curl_easy_setopt(_curl, CURLOPT_URL, _uri.c_str());
    _buffer.clear();
    _buffer.str("");
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, data.size());
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, data.size() > 0 ? data.c_str() : NULL);
    CURLcode res = curl_easy_perform(_curl);
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, NULL); // have to restore the default
    CHECKCURLCODE(res, "curl_easy_perform failed");
    long http_code = 0;
    res = curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
    CHECKCURLCODE(res, "curl_easy_getinfo failed");
    if( _buffer.rdbuf()->in_avail() > 0 ) {
        boost::property_tree::read_json(_buffer, pt);
    }
    if( expectedhttpcode != 0 && http_code != expectedhttpcode ) {
        std::string error_message = pt.get<std::string>("error_message", std::string());
        std::string traceback = pt.get<std::string>("traceback", std::string());
        throw MUJIN_EXCEPTION_FORMAT("HTTP POST to '%s' returned HTTP status %s: %s", relativeuri%http_code%error_message, MEC_HTTPServer);
    }
    return http_code;
}

void ControllerClientImpl::CallDelete(const std::string& relativeuri)
{
    boost::mutex::scoped_lock lock(_mutex);
    _uri = _baseapiuri;
    _uri += relativeuri;
    curl_easy_setopt(_curl, CURLOPT_URL, _uri.c_str());
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    CURLcode res = curl_easy_perform(_curl);
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, NULL); // have to restore the default
    CHECKCURLCODE(res, "curl_easy_perform failed");
    long http_code = 0;
    res = curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
    CHECKCURLCODE(res, "curl_easy_getinfo failed");
    if( http_code != 204 ) { // or 200 or 202 or 201?
        throw MUJIN_EXCEPTION_FORMAT("HTTP DELETE to '%s' returned HTTP status %s", relativeuri%http_code, MEC_HTTPServer);
    }
}

std::stringstream& ControllerClientImpl::GetBuffer()
{
    return _buffer;
}

void ControllerClientImpl::SetDefaultSceneType(const std::string& scenetype)
{
    _defaultscenetype = scenetype;
}

const std::string& ControllerClientImpl::GetDefaultSceneType()
{
    return _defaultscenetype;
}

void ControllerClientImpl::SetDefaultTaskType(const std::string& tasktype)
{
    _defaultscenetype = tasktype;
}

const std::string& ControllerClientImpl::GetDefaultTaskType()
{
    return _defaulttasktype;
}

std::string ControllerClientImpl::GetScenePrimaryKeyFromURI_UTF8(const std::string& uri)
{
    size_t index = uri.find(":/");
    MUJIN_ASSERT_OP_FORMAT(index,!=,std::string::npos, "bad URI: %s", uri, MEC_InvalidArguments);
    uri.substr(index+2);
    char* pcurlresult = curl_easy_escape(_curl, uri.c_str()+index+2,uri.size()-index-2);
    std::string sresult(pcurlresult);
    curl_free(pcurlresult); // have to release the result
    return sresult;
}

std::string ControllerClientImpl::GetScenePrimaryKeyFromURI_UTF16(const std::wstring& uri)
{
    std::string utf8line;
    utf8::utf16to8(uri.begin(), uri.end(), std::back_inserter(utf8line));
    return GetScenePrimaryKeyFromURI_UTF8(utf8line);
}

void ControllerClientImpl::GetProfile()
{
    _profile.clear();
    CallGet("profile/", _profile);
}

int ControllerClientImpl::_writer(char *data, size_t size, size_t nmemb, std::stringstream *writerData)
{
    if (writerData == NULL) {
        return 0;
    }
    writerData->write(data, size*nmemb);
    return size * nmemb;
}

void ControllerClientImpl::_SetHTTPHeaders()
{
    // set the header to only send json
    std::string s = std::string("Content-Type: application/json; charset=") + _charset;
    if( !!_httpheaders ) {
        curl_slist_free_all(_httpheaders);
    }
    _httpheaders = curl_slist_append(NULL, s.c_str());
    s = str(boost::format("Accept-Language: %s,en-us")%_language);
    _httpheaders = curl_slist_append(_httpheaders, s.c_str()); //,en;q=0.7,ja;q=0.3',")
    //_httpheaders = curl_slist_append(_httpheaders, "Accept:"); // necessary?
    s = std::string("X-CSRFToken: ")+_csrfmiddlewaretoken;
    _httpheaders = curl_slist_append(_httpheaders, s.c_str());
    _httpheaders = curl_slist_append(_httpheaders, "Connection: Keep-Alive");
    _httpheaders = curl_slist_append(_httpheaders, "Keep-Alive: 20"); // keep alive for 20s?
    // test on windows first
    //_httpheaders = curl_slist_append(_httpheaders, "Accept-Encoding: gzip, deflate");
    curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, _httpheaders);
}

std::string ControllerClientImpl::_GetCSRFFromCookies() {
    struct curl_slist *cookies;
    CURLcode res = curl_easy_getinfo(_curl, CURLINFO_COOKIELIST, &cookies);
    CHECKCURLCODE(res, "curl_easy_getinfo CURLINFO_COOKIELIST");
    struct curl_slist *nc = cookies;
    int i = 1;
    std::string csrfmiddlewaretoken;
    while (nc) {
        //std::cout << str(boost::format("[%d]: %s")%i%nc->data) << std::endl;
        char* csrftokenstart = strstr(nc->data, "csrftoken");
        if( !!csrftokenstart ) {
            std::stringstream ss(csrftokenstart+10);
            ss >> csrfmiddlewaretoken;
        }
        nc = nc->next;
        i++;
    }
    curl_slist_free_all(cookies);
    return csrfmiddlewaretoken;
}

std::string ControllerClientImpl::_EncodeWithoutSeparator(const std::string& raw)
{
    std::string output;
    size_t startindex = 0;
    for(size_t i = 0; i < raw.size(); ++i) {
        if( raw[i] == '/' ) {
            if( startindex != i ) {
                char* pescaped = curl_easy_escape(_curl, raw.c_str()+startindex, i-startindex);
                output += std::string(pescaped);
                curl_free(pescaped);
                startindex = i+1;
            }
            output += '/';
        }
    }
    if( startindex != raw.size() ) {
        char* pescaped = curl_easy_escape(_curl, raw.c_str()+startindex, raw.size()-startindex);
        output += std::string(pescaped);
        curl_free(pescaped);
    }
    return output;
}

void ControllerClientImpl::_EnsureWebDAVDirectories(const std::string& uriDestinationDir)
{
    std::list<std::string> listCreateDirs;
    std::string output;
    size_t startindex = 0;
    for(size_t i = 0; i < uriDestinationDir.size(); ++i) {
        if( uriDestinationDir[i] == '/' ) {
            if( startindex != i ) {
                char* pescaped = curl_easy_escape(_curl, uriDestinationDir.c_str()+startindex, i-startindex);
                listCreateDirs.push_back(std::string(pescaped));
                curl_free(pescaped);
                startindex = i+1;
            }
        }
    }
    if( startindex != uriDestinationDir.size() ) {
        char* pescaped = curl_easy_escape(_curl, uriDestinationDir.c_str()+startindex, uriDestinationDir.size()-startindex);
        listCreateDirs.push_back(std::string(pescaped));
        curl_free(pescaped);
    }

    // Check that the directory exists
    //curl_easy_setopt(_curl, CURLOPT_URL, buff);
    //curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    //res = curl_easy_perform(self->send_handle);
    //if(res != 0) {
    // // does not exist
    //}

    CurlCustomRequestSetter setter(_curl, "MKCOL");
    std::string totaluri = "";
    for(std::list<std::string>::iterator itdir = listCreateDirs.begin(); itdir != listCreateDirs.end(); ++itdir) {
        // first have to create the directory structure up to destinationdir
        if( totaluri.size() > 0 ) {
            totaluri += '/';
        }
        totaluri += *itdir;
        _uri = _basewebdavuri + totaluri;
        curl_easy_setopt(_curl, CURLOPT_URL, _uri.c_str());
        CURLcode res = curl_easy_perform(_curl);
        CHECKCURLCODE(res, "curl_easy_perform failed");
        long http_code = 0;
        res=curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
        /* creating directories

           Responses from a MKCOL request MUST NOT be cached as MKCOL has non-idempotent semantics.

           201 (Created) - The collection or structured resource was created in its entirety.

           403 (Forbidden) - This indicates at least one of two conditions: 1) the server does not allow the creation of collections at the given location in its namespace, or 2) the parent collection of the Request-URI exists but cannot accept members.

           405 (Method Not Allowed) - MKCOL can only be executed on a deleted/non-existent resource.

           409 (Conflict) - A collection cannot be made at the Request-URI until one or more intermediate collections have been created.

           415 (Unsupported Media Type)- The server does not support the request type of the body.

           507 (Insufficient Storage) - The resource does not have sufficient space to record the state of the resource after the execution of this method.

         */
        if( http_code != 201 && http_code != 301 ) {
            throw MUJIN_EXCEPTION_FORMAT("HTTP MKCOL failed with HTTP status %d: %s", http_code%_errormessage, MEC_HTTPServer);
        }
    }
}

void ControllerClientImpl::_UploadDirectoryToWebDAV_UTF8(const std::string& copydir, const std::string& uri)
{
    {
        // make sure the directory is created
        CurlCustomRequestSetter setter(_curl, "MKCOL");
        curl_easy_setopt(_curl, CURLOPT_URL, uri.c_str());
        CURLcode res = curl_easy_perform(_curl);
        CHECKCURLCODE(res, "curl_easy_perform failed");
        long http_code = 0;
        res=curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
        if( http_code != 201 && http_code != 301 ) {
            throw MUJIN_EXCEPTION_FORMAT("HTTP MKCOL failed for %s with HTTP status %d: %s", uri%http_code%_errormessage, MEC_HTTPServer);
        }
    }

    std::string sCopyDir_FS = encoding::ConvertUTF8ToFileSystemEncoding(copydir);
    std::cout << "uploading " << sCopyDir_FS << " -> " << uri << std::endl;

#if defined(_WIN32) || defined(_WIN64)
    WIN32_FIND_DATAA ffd;
    std::string searchstr = sCopyDir_FS + std::string("\\*");
    HANDLE hFind = FindFirstFileA(searchstr.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        throw MUJIN_EXCEPTION_FORMAT("could not retrieve file data for %s", sCopyDir_FS, MEC_Assert);
    }

    do {
        std::string filename = std::string(ffd.cFileName);
        if( filename != "." && filename != ".." ) {
            std::string filename_utf8 = encoding::ConvertMBStoUTF8(filename);
            std::string newcopydir = str(boost::format("%s\\%s")%copydir%filename_utf8);
            char* pescapeddir = curl_easy_escape(_curl, filename_utf8.c_str(), filename_utf8.size());
            std::string newuri = str(boost::format("%s/%s")%uri%pescapeddir);
            curl_free(pescapeddir);

            if( ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
                _UploadDirectoryToWebDAV_UTF8(newcopydir, newuri);
            }
            else if( ffd.dwFileAttributes == 0 || ffd.dwFileAttributes == FILE_ATTRIBUTE_READONLY || ffd.dwFileAttributes == FILE_ATTRIBUTE_NORMAL || ffd.dwFileAttributes == FILE_ATTRIBUTE_ARCHIVE ) {
                _UploadFileToWebDAV_UTF8(newcopydir, newuri);
            }
        }
    } while(FindNextFileA(hFind,&ffd) != 0);

    DWORD err = GetLastError();
    FindClose(hFind);
    if( err != ERROR_NO_MORE_FILES ) {
        throw MUJIN_EXCEPTION_FORMAT("system error 0x%x when recursing through %s", err%sCopyDir_FS, MEC_HTTPServer);
    }

#else
    boost::filesystem::path bfpcopydir(copydir);
    for(boost::filesystem::directory_iterator itdir(bfpcopydir); itdir != boost::filesystem::directory_iterator(); ++itdir) {
#if defined(BOOST_FILESYSTEM_VERSION) && BOOST_FILESYSTEM_VERSION >= 3
        std::string dirfilename = encoding::ConvertFileSystemEncodingToUTF8(itdir->path().filename().string());
#else
        std::string dirfilename = encoding::ConvertFileSystemEncodingToUTF8(itdir->path().filename());
#endif
        char* pescapeddir = curl_easy_escape(_curl, dirfilename.c_str(), dirfilename.size());
        std::string newuri = str(boost::format("%s/%s")%uri%pescapeddir);
        curl_free(pescapeddir);
        if( boost::filesystem::is_directory(itdir->status()) ) {
            _UploadDirectoryToWebDAV_UTF8(itdir->path().string(), newuri);
        }
        else if( boost::filesystem::is_regular_file(itdir->status()) ) {
            _UploadFileToWebDAV_UTF8(itdir->path().string(), newuri);
        }
    }
#endif // defined(_WIN32) || defined(_WIN64)
}

void ControllerClientImpl::_UploadDirectoryToWebDAV_UTF16(const std::wstring& copydir, const std::string& uri)
{
    {
        // make sure the directory is created
        CurlCustomRequestSetter setter(_curl, "MKCOL");
        curl_easy_setopt(_curl, CURLOPT_URL, uri.c_str());
        CURLcode res = curl_easy_perform(_curl);
        CHECKCURLCODE(res, "curl_easy_perform failed");
        long http_code = 0;
        res=curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
        if( http_code != 201 && http_code != 301 ) {
            throw MUJIN_EXCEPTION_FORMAT("HTTP MKCOL failed for %s with HTTP status %d: %s", uri%http_code%_errormessage, MEC_HTTPServer);
        }
    }

    std::wstring sCopyDir_FS = copydir;
    std::cout << "uploading " << encoding::ConvertUTF16ToFileSystemEncoding(copydir) << " -> " << uri << std::endl;

#if defined(_WIN32) || defined(_WIN64)
    WIN32_FIND_DATAW ffd;
    std::wstring searchstr = sCopyDir_FS + std::wstring(L"\\*");
    HANDLE hFind = FindFirstFileW(searchstr.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE)  {
        throw MUJIN_EXCEPTION_FORMAT("could not retrieve file data for %s", encoding::ConvertUTF16ToFileSystemEncoding(copydir), MEC_Assert);
    }

    do {
        std::wstring filename = std::wstring(ffd.cFileName);
        if( filename != L"." && filename != L".." ) {
            std::string filename_utf8;
            utf8::utf16to8(filename.begin(), filename.end(), std::back_inserter(filename_utf8));
            std::wstring newcopydir = str(boost::wformat(L"%s\\%s")%copydir%filename);
            char* pescapeddir = curl_easy_escape(_curl, filename_utf8.c_str(), filename_utf8.size());
            std::string newuri = str(boost::format("%s/%s")%uri%pescapeddir);
            curl_free(pescapeddir);

            if( ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
                _UploadDirectoryToWebDAV_UTF16(newcopydir, newuri);
            }
            else if( ffd.dwFileAttributes == 0 || ffd.dwFileAttributes == FILE_ATTRIBUTE_READONLY || ffd.dwFileAttributes == FILE_ATTRIBUTE_NORMAL || ffd.dwFileAttributes == FILE_ATTRIBUTE_ARCHIVE ) {
                _UploadFileToWebDAV_UTF16(newcopydir, newuri);
            }
        }
    } while(FindNextFileW(hFind,&ffd) != 0);

    DWORD err = GetLastError();
    FindClose(hFind);
    if( err !=  ERROR_NO_MORE_FILES ) {
        throw MUJIN_EXCEPTION_FORMAT("system error 0x%x when recursing through %s", err%encoding::ConvertUTF16ToFileSystemEncoding(copydir), MEC_HTTPServer);
    }

#else
    boost::filesystem::wpath bfpcopydir(copydir);
    for(boost::filesystem::wdirectory_iterator itdir(bfpcopydir); itdir != boost::filesystem::wdirectory_iterator(); ++itdir) {
#if defined(BOOST_FILESYSTEM_VERSION) && BOOST_FILESYSTEM_VERSION >= 3
        std::wstring dirfilename_utf16 = itdir->path().filename().string();
#else
        std::wstring dirfilename_utf16 = itdir->path().filename();
#endif
        std::string dirfilename;
        utf8::utf16to8(dirfilename_utf16.begin(), dirfilename_utf16.end(), std::back_inserter(dirfilename));
        char* pescapeddir = curl_easy_escape(_curl, dirfilename.c_str(), dirfilename.size());
        std::string newuri = str(boost::format("%s/%s")%uri%pescapeddir);
        curl_free(pescapeddir);
        if( boost::filesystem::is_directory(itdir->status()) ) {
            _UploadDirectoryToWebDAV_UTF16(itdir->path().string(), newuri);
        }
        else if( boost::filesystem::is_regular_file(itdir->status()) ) {
            _UploadFileToWebDAV_UTF16(itdir->path().string(), newuri);
        }
    }
#endif // defined(_WIN32) || defined(_WIN64)
}

void ControllerClientImpl::_UploadFileToWebDAV_UTF8(const std::string& filename, const std::string& uri)
{
    std::string sFilename_FS = encoding::ConvertUTF8ToFileSystemEncoding(filename);
    FileHandler handler(sFilename_FS.c_str());
    if(!handler._fd) {
        throw MUJIN_EXCEPTION_FORMAT("failed to open filename %s for uploading", sFilename_FS, MEC_InvalidArguments);
    }
    _UploadFileToWebDAV(handler._fd, uri);
}

void ControllerClientImpl::_UploadFileToWebDAV_UTF16(const std::wstring& filename, const std::string& uri)
{
    std::string filename_fs = encoding::ConvertUTF16ToFileSystemEncoding(filename);
#if defined(_WIN32) || defined(_WIN64)
    FileHandler handler(filename.c_str());
#else
    // linux does not support wide-char fopen
    FileHandler handler(filename_fs.c_str());
#endif
    if(!handler._fd) {
        throw MUJIN_EXCEPTION_FORMAT("failed to open filename %s for uploading", filename_fs, MEC_InvalidArguments);
    }
    _UploadFileToWebDAV(handler._fd, uri);
}

void ControllerClientImpl::_UploadFileToWebDAV(FILE* fd, const std::string& uri)
{
#if defined(_WIN32) || defined(_WIN64)
    fseek(fd,0,SEEK_END);
    curl_off_t filesize = ftell(fd);
    fseek(fd,0,SEEK_SET);
#else
    // to get the file size
    struct stat file_info;
    if(fstat(fileno(fd), &file_info) != 0) {
        throw MUJIN_EXCEPTION_FORMAT("failed to stat %s for filesize", uri, MEC_InvalidArguments);
    }
    curl_off_t filesize = (curl_off_t)file_info.st_size;
#endif

    // tell it to "upload" to the URL
    CurlUploadSetter uploadsetter(_curl);
    curl_easy_setopt(_curl, CURLOPT_HTTPGET, 0L);
    curl_easy_setopt(_curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(_curl, CURLOPT_READDATA, fd);
    curl_easy_setopt(_curl, CURLOPT_INFILESIZE_LARGE, filesize);
    //curl_easy_setopt(_curl, CURLOPT_NOBODY, 1L);
#if defined(_WIN32) || defined(_WIN64)
    curl_easy_setopt(_curl, CURLOPT_READFUNCTION, _ReadUploadCallback);
#endif

    CURLcode res = curl_easy_perform(_curl);
    CHECKCURLCODE(res, "curl_easy_perform failed");
    long http_code = 0;
    res=curl_easy_getinfo (_curl, CURLINFO_RESPONSE_CODE, &http_code);
    // 204 is when it overwrites the file?
    if( http_code != 201 && http_code != 204 ) {
        if( http_code == 400 ) {
            throw MUJIN_EXCEPTION_FORMAT("upload of %s failed with HTTP status %s, perhaps file exists already?", uri%http_code, MEC_HTTPServer);
        }
        else {
            throw MUJIN_EXCEPTION_FORMAT("upload of %s failed with HTTP status %s", uri%http_code, MEC_HTTPServer);
        }
    }
    // now extract transfer info
    //double speed_upload, total_time;
    //curl_easy_getinfo(_curl, CURLINFO_SPEED_UPLOAD, &speed_upload);
    //curl_easy_getinfo(_curl, CURLINFO_TOTAL_TIME, &total_time);
    //printf("http code: %d, Speed: %.3f bytes/sec during %.3f seconds\n", http_code, speed_upload, total_time);
}

size_t ControllerClientImpl::_ReadUploadCallback(void *ptr, size_t size, size_t nmemb, void *stream)
{
    curl_off_t nread;
    // in real-world cases, this would probably get this data differently as this fread() stuff is exactly what the library already would do by default internally
    size_t retcode = fread(ptr, size, nmemb, (FILE*)stream);

    nread = (curl_off_t)retcode;
    //fprintf(stderr, "*** We read %" CURL_FORMAT_CURL_OFF_T " bytes from file\n", nread);
    return retcode;
}

} // end namespace mujinclient