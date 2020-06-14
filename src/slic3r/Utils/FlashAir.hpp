#ifndef slic3r_FlashAir_hpp_
#define slic3r_FlashAir_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"


namespace Slic3r {

class DynamicPrintConfig;
class Http;

class FlashAir : public PrintHost
{
public:
	FlashAir(DynamicPrintConfig *config);
	~FlashAir() override = default;

	const char* get_name() const override;

	bool test(wxString &curl_msg) const override;
	wxString get_test_ok_msg() const override;
	wxString get_test_failed_msg(wxString &msg) const override;
	bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn) const override;
	bool has_auto_discovery() const override { return false; }
	bool can_test() const override { return true; }
	bool can_start_print() const override { return false; }
    bool can_support_multiple_printers() const override { return false; }
	std::string get_host() const override { return host; }
    
    bool get_groups(wxArrayString &groups) const override { return false; }
    bool get_printers(wxArrayString &printers) const override { return false; }
    
private:
	std::string host;

	std::string timestamp_str() const;
	std::string make_url(const std::string &path) const;
	std::string make_url(const std::string &path, const std::string &arg, const std::string &val) const;
};

}

#endif
