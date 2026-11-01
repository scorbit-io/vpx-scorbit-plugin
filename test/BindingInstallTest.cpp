// license:GPLv3+

// The add-only install of the bundled table-script binding into <prefPath>/scripts.

#include "BindingInstall.h"
#include "Check.h"

#include <fstream>
#include <iterator>
#include <random>
#include <string>

using namespace Scorbit;
namespace fs = std::filesystem;

namespace
{

fs::path FreshDir(const char* name)
{
   std::random_device rd;
   const fs::path dir = fs::temp_directory_path() / ("scorbit-binding-" + std::string(name) + "-" + std::to_string(rd()));
   fs::remove_all(dir);
   fs::create_directories(dir);
   return dir;
}

void Write(const fs::path& path, const std::string& bytes)
{
   std::ofstream(path, std::ios::binary) << bytes;
}

std::string Read(const fs::path& path)
{
   std::ifstream in(path, std::ios::binary);
   return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

size_t Entries(const fs::path& dir)
{
   size_t n = 0;
   for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir))
      n++;
   return n;
}

}

int main()
{
   const fs::path root = FreshDir("test");
   const fs::path bundle = root / "plugin";
   fs::create_directories(bundle);
   const fs::path source = bundle / "scorbit-2.0.6.vbs";
   Write(source, "' binding 2.0.6\r\n");

   // Missing: added, with the bundled bytes, and no temporary file left.
   {
      const fs::path scripts = root / "prefs1" / "scripts"; // the folder itself does not exist yet
      const BindingInstallResult r = InstallBinding(source, scripts);
      CHECK(r.outcome == BindingInstall::Added);
      CHECK(Read(scripts / "scorbit-2.0.6.vbs") == "' binding 2.0.6\r\n");
      CHECK(Entries(scripts) == 1);
      // Again: now present, untouched.
      CHECK(InstallBinding(source, scripts).outcome == BindingInstall::Present);
      CHECK(Entries(scripts) == 1);
   }

   // Present with that exact name: left as it is, even though its bytes differ.
   {
      const fs::path scripts = root / "prefs2" / "scripts";
      fs::create_directories(scripts);
      Write(scripts / "scorbit-2.0.6.vbs", "' an author's edited copy\r\n");
      const BindingInstallResult r = InstallBinding(source, scripts);
      CHECK(r.outcome == BindingInstall::Present);
      CHECK(Read(scripts / "scorbit-2.0.6.vbs") == "' an author's edited copy\r\n");
      CHECK(Entries(scripts) == 1);
   }

   // Other versions present: the new one is added beside them, and they are left as they are.
   {
      const fs::path scripts = root / "prefs3" / "scripts";
      fs::create_directories(scripts);
      Write(scripts / "scorbit-2.0.5.vbs", "' binding 2.0.5\r\n");
      Write(scripts / "scorbit-1.0.0.vbs", "' binding 1.0.0\r\n");
      Write(scripts / "core.vbs", "' VPX's own\r\n");
      const BindingInstallResult r = InstallBinding(source, scripts);
      CHECK(r.outcome == BindingInstall::Added);
      CHECK(Read(scripts / "scorbit-2.0.6.vbs") == "' binding 2.0.6\r\n");
      CHECK(Read(scripts / "scorbit-2.0.5.vbs") == "' binding 2.0.5\r\n");
      CHECK(Read(scripts / "scorbit-1.0.0.vbs") == "' binding 1.0.0\r\n");
      CHECK(Read(scripts / "core.vbs") == "' VPX's own\r\n");
      CHECK(Entries(scripts) == 4);
   }

   // An unwritable scripts folder: here a file stands where the folder should be. Failed, with a
   // reason, nothing thrown and nothing changed.
   {
      const fs::path prefs = root / "prefs4";
      fs::create_directories(prefs);
      Write(prefs / "scripts", "not a folder");
      const BindingInstallResult r = InstallBinding(source, prefs / "scripts");
      CHECK(r.outcome == BindingInstall::Failed);
      CHECK(!r.detail.empty());
      CHECK(Read(prefs / "scripts") == "not a folder");
   }

   // The bundled file missing: failed, nothing created.
   {
      const fs::path scripts = root / "prefs5" / "scripts";
      const BindingInstallResult r = InstallBinding(bundle / "scorbit-9.9.9.vbs", scripts);
      CHECK(r.outcome == BindingInstall::Failed);
      CHECK(!fs::exists(scripts));
   }

   std::error_code ignored;
   fs::remove_all(root, ignored);
   return ScorbitTest::Summary("binding_install");
}
