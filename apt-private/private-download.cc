// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/acquire.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/cacheset.h>
#include <apt-pkg/cmndline.h>
#include <apt-pkg/clean.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/error.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/strutl.h>

#include <apt-private/private-cachefile.h>
#include <apt-private/private-download.h>
#include <apt-private/private-output.h>
#include <apt-private/private-utils.h>
#include <apt-private/acqprogress.h>

#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <fcntl.h>
#ifdef HAVE_VFS_H
#include <sys/vfs.h>
#else
#ifdef HAVE_PARAMS_H
#include <sys/params.h>
#endif
#include <sys/mount.h>
#endif
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <errno.h>

#include <apti18n.h>
									/*}}}*/

// CheckAuth - check if each download comes form a trusted source	/*{{{*/
bool CheckAuth(pkgAcquire& Fetcher, bool const PromptUser)
{
   std::vector<std::string> UntrustedList;
   for (pkgAcquire::ItemIterator I = Fetcher.ItemsBegin(); I < Fetcher.ItemsEnd(); ++I)
      if (!(*I)->IsTrusted())
	 UntrustedList.push_back((*I)->ShortDesc());

   if (UntrustedList.empty())
      return true;

   return AuthPrompt(UntrustedList, PromptUser);
}
									/*}}}*/
bool AuthPrompt(std::vector<std::string> const &UntrustedList, bool const PromptUser)/*{{{*/
{
   ShowList(c2out,_("WARNING: The following packages cannot be authenticated!"), UntrustedList,
	 [](std::string const&) { return true; },
	 [](std::string const&str) { return str; },
	 [](std::string const&) { return ""; });

   if (_config->FindB("APT::Get::AllowUnauthenticated",false) == true)
   {
      c2out << _("Authentication warning overridden.\n");
      return true;
   }

   if (PromptUser == false)
      return _error->Error(_("Some packages could not be authenticated"));

   if (_config->FindI("quiet",0) < 2
       && _config->FindB("APT::Get::Assume-Yes",false) == false)
   {
      if (!YnPrompt(_("Install these packages without verification?"), false))
         return _error->Error(_("Some packages could not be authenticated"));

      return true;
   }
   else if (_config->FindB("APT::Get::Force-Yes",false) == true) {
      _error->Warning(_("--force-yes is deprecated, use one of the options starting with --allow instead."));
      return true;
   }

   return _error->Error(_("There were unauthenticated packages and -y was used without --allow-unauthenticated"));
}

// GetOutput - execute CmdLine and place the first line in output	/*{{{*/
static bool GetOutput(std::string &output, std::string const CmdLine, bool const Debug)
{
   pid_t Child;
   FileFd PipeFd;
   char buf[1024];

   if (Debug)
      std::cerr << CmdLine << std::endl;

   std::vector<const char *> Args = {"/bin/sh", "-c", CmdLine.c_str(), nullptr};
   if (Popen(&Args[0], PipeFd, Child, FileFd::ReadOnly, false) == false)
      return false;

   PipeFd.ReadLine(buf, sizeof(buf));
   buf[sizeof(buf) - 1] = '\0';
   PipeFd.Close();

   if (ExecWait(Child, "sh") == false)
      return false;

   output = _strstrip(buf);

   return true;
}

// CheckReproducible - check if each download comes form a reproducible source	/*{{{*/
bool CheckReproducible(pkgAcquire& Fetcher, bool const PromptUser)
{
   if (_config->FindB("APT::Get::AllowUnreproducible", false))
      return true;

   std::string Output;
   std::vector<std::string> UnreproducibleList;
   bool const Debug = _config->FindB("Debug::pkgAcquire::Reproducible",false);

   std::string const Url = _config->Find("APT::Get::ReproducibleStatusJsonUrl",
      "https://tests.reproducible-builds.org/reproducible.json.bz2");
   std::string const NativeArch = _config->Find("APT::Architecture");
   std::string const CacheFileName = _config->FindFile("Dir::Cache::reproduciblecache");
   std::string const DefaultRelease = _config->Find("APT::Default-Release","unstable");

   // Update local status file
   std::string const UpdateCommand =
      std::string("/usr/bin/curl")
      + ((Debug) ? "" : " --silent")
      + " --location"
      + " -z " + CacheFileName
      + " -o " + CacheFileName
      + " " + Url;
   if (GetOutput(Output, UpdateCommand, Debug) == false)
      return _error->Error(_("Could not update reproducible cache"));

   for (pkgAcquire::ItemIterator I = Fetcher.ItemsBegin(); I < Fetcher.ItemsEnd(); ++I) {
      std::string const BinaryPkg = (*I)->ShortDesc();
      std::string SrcPkg = BinaryPkg;

      if (Debug)
         std::cerr << "Checking reproducibility of " << BinaryPkg << std::endl;

      // Determine source package name
      std::string const SourcePackageCommand =
         "apt-cache show " + BinaryPkg + " | awk '/Source: / { print $2 }'";
      if (GetOutput(Output, SourcePackageCommand, Debug) == false)
         return _error->Error(_("Could not check source package name"));

      // If we got output, update the source package name
      if (Output.length() > 0)
         SrcPkg = Output;

      std::string const JqCommand =
         "bunzip2 -c " + CacheFileName + " | " +
         "jq --compact-output --raw-output '.[] | " +
            "select(.suite==\"" + DefaultRelease + "\") | " +
            "select(.package==\"" + SrcPkg + "\") | " +
            "select(.status==\"reproducible\") | " +
            "select(.architecture==\"" + NativeArch + "\")" +
            "'";
      if (GetOutput(Output, JqCommand, Debug) == false)
         return _error->Error(_("Could not filter reproducible status"));

      // If we got no output, we failed to match filters
      if (Output.length() == 0)
         UnreproducibleList.push_back(BinaryPkg);
   }

   if (UnreproducibleList.empty())
      return true;

   return ReproduciblePrompt(UnreproducibleList, PromptUser);
}
									/*}}}*/


bool ReproduciblePrompt(std::vector<std::string> const &UnreproducibleList, bool const PromptUser)/*{{{*/
{
   ShowList(c2out,_("WARNING: The following packages are not reproducible!"), UnreproducibleList,
	 [](std::string const&) { return true; },
	 [](std::string const&str) { return str; },
	 [](std::string const&) { return ""; });

   if (_config->FindB("APT::Get::AllowUnreproducible",false) == true)
   {
      c2out << _("Unreproducible warning overridden.\n");
      return true;
   }

   if (PromptUser == false)
      return _error->Error(_("Some packages are not reproducible"));

   if (_config->FindI("quiet",0) < 2
       && _config->FindB("APT::Get::Assume-Yes",false) == false)
   {
      if (!YnPrompt(_("Install these packages anyway?"), false))
         return _error->Error(_("Some packages are not reproducible"));

      return true;
   }
   else if (_config->FindB("APT::Get::Force-Yes",false) == true) {
      _error->Warning(_("--force-yes is deprecated, use one of the options starting with --allow instead."));
      return true;
   }

   return _error->Error(_("There were unreproducible packages and -y was used without --allow-unreproducible"));
}
									/*}}}*/
bool AcquireRun(pkgAcquire &Fetcher, int const PulseInterval, bool * const Failure, bool * const TransientNetworkFailure)/*{{{*/
{
   pkgAcquire::RunResult res;
   if(PulseInterval > 0)
      res = Fetcher.Run(PulseInterval);
   else
      res = Fetcher.Run();

   if (res == pkgAcquire::Failed)
      return false;

   for (pkgAcquire::ItemIterator I = Fetcher.ItemsBegin();
	I != Fetcher.ItemsEnd(); ++I)
   {

      if ((*I)->Status == pkgAcquire::Item::StatDone &&
	    (*I)->Complete == true)
	 continue;

      if (TransientNetworkFailure != NULL && (*I)->Status == pkgAcquire::Item::StatIdle)
      {
	 *TransientNetworkFailure = true;
	 continue;
      }

      ::URI uri((*I)->DescURI());
      uri.User.clear();
      uri.Password.clear();
      std::string descUri = std::string(uri);
      _error->Error(_("Failed to fetch %s  %s"), descUri.c_str(),
	    (*I)->ErrorText.c_str());

      if (Failure != NULL)
	 *Failure = true;
   }

   return true;
}
									/*}}}*/
bool CheckFreeSpaceBeforeDownload(std::string const &Dir, unsigned long long FetchBytes)/*{{{*/
{
   uint32_t const RAMFS_MAGIC = 0x858458f6;
   /* Check for enough free space, but only if we are actually going to
      download */
   if (_config->FindB("APT::Get::Print-URIs", false) == true ||
       _config->FindB("APT::Get::Download", true) == false)
      return true;

   struct statvfs Buf;
   if (statvfs(Dir.c_str(),&Buf) != 0) {
      if (errno == EOVERFLOW)
	 return _error->WarningE("statvfs",_("Couldn't determine free space in %s"),
	       Dir.c_str());
      else
	 return _error->Errno("statvfs",_("Couldn't determine free space in %s"),
	       Dir.c_str());
   }
   else
   {
      unsigned long long const FreeBlocks = _config->Find("APT::Sandbox::User").empty() ? Buf.f_bfree : Buf.f_bavail;
      if (FreeBlocks < (FetchBytes / Buf.f_bsize))
      {
	 struct statfs Stat;
	 if (statfs(Dir.c_str(),&Stat) != 0
#ifdef HAVE_STRUCT_STATFS_F_TYPE
	       || Stat.f_type != RAMFS_MAGIC
#endif
	    )
	    return _error->Error(_("You don't have enough free space in %s."),
		  Dir.c_str());
      }
   }
   return true;
}
									/*}}}*/

aptAcquireWithTextStatus::aptAcquireWithTextStatus() : pkgAcquire::pkgAcquire(),
   Stat(std::cout, ScreenWidth, _config->FindI("quiet",0))
{
   SetLog(&Stat);
}

// DoDownload - download a binary					/*{{{*/
bool DoDownload(CommandLine &CmdL)
{
   CacheFile Cache;
   if (Cache.ReadOnlyOpen() == false)
      return false;

   APT::CacheSetHelper helper;
   APT::VersionSet verset = APT::VersionSet::FromCommandLine(Cache,
		CmdL.FileList + 1, APT::CacheSetHelper::CANDIDATE, helper);

   if (verset.empty() == true)
      return false;

   pkgRecords Recs(Cache);
   pkgSourceList *SrcList = Cache.GetSourceList();

   // reuse the usual acquire methods for deb files, but don't drop them into
   // the usual directories - keep everything in the current directory
   aptAcquireWithTextStatus Fetcher;
   std::vector<std::string> storefile(verset.size());
   std::string const cwd = SafeGetCWD();
   _config->Set("Dir::Cache::Archives", cwd);
   int i = 0;
   for (APT::VersionSet::const_iterator Ver = verset.begin();
	 Ver != verset.end(); ++Ver, ++i)
   {
      pkgAcquire::Item *I = new pkgAcqArchive(&Fetcher, SrcList, &Recs, *Ver, storefile[i]);
      if (storefile[i].empty())
	 continue;
      std::string const filename = cwd + flNotDir(storefile[i]);
      storefile[i].assign(filename);
      I->DestFile.assign(filename);
   }

   // Just print out the uris and exit if the --print-uris flag was used
   if (_config->FindB("APT::Get::Print-URIs") == true)
   {
      pkgAcquire::UriIterator I = Fetcher.UriBegin();
      for (; I != Fetcher.UriEnd(); ++I)
	 std::cout << '\'' << I->URI << "' " << flNotDir(I->Owner->DestFile)  << ' ' <<
	       I->Owner->FileSize << ' ' << I->Owner->HashSum() << std::endl;
      return true;
   }

   if (_error->PendingError() == true
       || CheckAuth(Fetcher, false) == false
       || CheckReproducible(Fetcher, false) == false)
      return false;

   bool Failed = false;
   if (AcquireRun(Fetcher, 0, &Failed, NULL) == false)
      return false;

   // copy files in local sources to the current directory
   for (pkgAcquire::ItemIterator I = Fetcher.ItemsBegin(); I != Fetcher.ItemsEnd(); ++I)
   {
      std::string const filename = cwd + flNotDir((*I)->DestFile);
      if ((*I)->Local == true &&
          filename != (*I)->DestFile &&
          (*I)->Status == pkgAcquire::Item::StatDone)
      {
	 std::ifstream src((*I)->DestFile.c_str(), std::ios::binary);
	 std::ofstream dst(filename.c_str(), std::ios::binary);
	 dst << src.rdbuf();
	 chmod(filename.c_str(), 0644);
      }
   }
   return Failed == false;
}
									/*}}}*/
// DoChangelog - Get changelog from the command line			/*{{{*/
bool DoChangelog(CommandLine &CmdL)
{
   CacheFile Cache;
   if (Cache.ReadOnlyOpen() == false)
      return false;

   APT::CacheSetHelper helper;
   APT::VersionList verset = APT::VersionList::FromCommandLine(Cache,
		CmdL.FileList + 1, APT::CacheSetHelper::CANDIDATE, helper);
   if (verset.empty() == true)
      return false;

   bool const downOnly = _config->FindB("APT::Get::Download-Only", false);
   bool const printOnly = _config->FindB("APT::Get::Print-URIs", false);
   if (printOnly)
      _config->CndSet("Acquire::Changelogs::AlwaysOnline", true);

   aptAcquireWithTextStatus Fetcher;
   for (APT::VersionList::const_iterator Ver = verset.begin();
        Ver != verset.end();
        ++Ver)
   {
      if (printOnly)
	 new pkgAcqChangelog(&Fetcher, Ver, "/dev/null");
      else if (downOnly)
	 new pkgAcqChangelog(&Fetcher, Ver, ".");
      else
	 new pkgAcqChangelog(&Fetcher, Ver);
   }

   if (printOnly == false)
   {
      bool Failed = false;
      if (AcquireRun(Fetcher, 0, &Failed, NULL) == false || Failed == true)
	 return false;
   }

   if (downOnly == false || printOnly == true)
   {
      bool Failed = false;
      for (pkgAcquire::ItemIterator I = Fetcher.ItemsBegin(); I != Fetcher.ItemsEnd(); ++I)
      {
	 if (printOnly)
	 {
	    if ((*I)->ErrorText.empty() == false)
	    {
	       Failed = true;
	       _error->Error("%s", (*I)->ErrorText.c_str());
	    }
	    else
	       std::cout << '\'' << (*I)->DescURI() << "' " << flNotDir((*I)->DestFile)  << std::endl;
	 }
	 else
	    DisplayFileInPager((*I)->DestFile);
      }
      return Failed == false;
   }

   return true;
}
									/*}}}*/

// DoClean - Remove download archives					/*{{{*/
bool DoClean(CommandLine &)
{
   std::string const archivedir = _config->FindDir("Dir::Cache::archives");
   std::string const listsdir = _config->FindDir("Dir::state::lists");

   if (_config->FindB("APT::Get::Simulate") == true)
   {
      std::string const pkgcache = _config->FindFile("Dir::cache::pkgcache");
      std::string const srcpkgcache = _config->FindFile("Dir::cache::srcpkgcache");
      std::cout << "Del " << archivedir << "* " << archivedir << "partial/*"<< std::endl
	   << "Del " << listsdir << "partial/*" << std::endl
	   << "Del " << pkgcache << " " << srcpkgcache << std::endl;
      return true;
   }

   pkgAcquire Fetcher;
   if (archivedir.empty() == false && FileExists(archivedir) == true &&
	 Fetcher.GetLock(archivedir) == true)
   {
      Fetcher.Clean(archivedir);
      Fetcher.Clean(archivedir + "partial/");
   }

   if (listsdir.empty() == false && FileExists(listsdir) == true &&
	 Fetcher.GetLock(listsdir) == true)
   {
      Fetcher.Clean(listsdir + "partial/");
   }

   pkgCacheFile::RemoveCaches();

   return true;
}
									/*}}}*/
// DoAutoClean - Smartly remove downloaded archives			/*{{{*/
// ---------------------------------------------------------------------
/* This is similar to clean but it only purges things that cannot be 
   downloaded, that is old versions of cached packages. */
 class LogCleaner : public pkgArchiveCleaner
{
   protected:
      virtual void Erase(const char *File, std::string Pkg, std::string Ver,struct stat &St) APT_OVERRIDE
      {
	 c1out << "Del " << Pkg << " " << Ver << " [" << SizeToStr(St.st_size) << "B]" << std::endl;

	 if (_config->FindB("APT::Get::Simulate") == false)
	    RemoveFile("Cleaner::Erase", File);
      };
};
bool DoAutoClean(CommandLine &)
{
   std::string const archivedir = _config->FindDir("Dir::Cache::Archives");
   if (FileExists(archivedir) == false)
      return true;

   // Lock the archive directory
   FileFd Lock;
   if (_config->FindB("Debug::NoLocking",false) == false)
   {
      int lock_fd = GetLock(flCombine(archivedir, "lock"));
      if (lock_fd < 0)
	 return _error->Error(_("Unable to lock the download directory"));
      Lock.Fd(lock_fd);
   }

   CacheFile Cache;
   if (Cache.Open(false) == false)
      return false;

   LogCleaner Cleaner;

   return Cleaner.Go(archivedir, *Cache) &&
      Cleaner.Go(flCombine(archivedir, "partial/"), *Cache);
}
									/*}}}*/
