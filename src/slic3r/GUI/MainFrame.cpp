#include "MainFrame.hpp"

#include <wx/debug.h>
#include <wx/filename.h>
//#include <wx/glcanvas.h>
#include <wx/icon.h>
#include <wx/menu.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/tooltip.h>

#include <boost/algorithm/string/predicate.hpp>

#include "libslic3r/Polygon.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Time.hpp"

#include "../Utils/Process.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "InstanceCheck.hpp"
#include "Mouse3DController.hpp"
#include "Plater.hpp"
#include "PrintHostDialogs.hpp"
#include "ProgressStatusBar.hpp"
#include "RemovableDriveManager.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"

#include <fstream>
#include <string_view>

#include "GUI_App.hpp"

#ifdef _WIN32
#include <dbt.h>
#include <shlobj.h>
#endif // _WIN32

namespace Slic3r {
namespace GUI {

enum class ERescaleTarget
{
    Mainframe,
    SettingsDialog
};

#ifdef __APPLE__
class PrusaSlicerTaskBarIcon : public wxTaskBarIcon
{
public:
    PrusaSlicerTaskBarIcon(wxTaskBarIconType iconType = wxTBI_DEFAULT_TYPE) : wxTaskBarIcon(iconType) {}
    wxMenu *CreatePopupMenu() override {
        wxMenu *menu = new wxMenu;
        if(wxGetApp().app_config->get("single_instance") == "0") {
            // Only allow opening a new Slic3r instance on OSX if "single_instance" is disabled, 
            // as starting new instances would interfere with the locking mechanism of "single_instance" support.
            append_menu_item(menu, wxID_ANY, _L("Open new instance"), _L("Open a new Slic3r instance"),
            [this](wxCommandEvent&) { start_new_slicer(); }, "", nullptr);
        }
        append_menu_item(menu, wxID_ANY, _L("G-code preview") + dots, _L("Open G-code viewer"),
            [this](wxCommandEvent&) { start_new_gcodeviewer_open_file(); }, "", nullptr);
        return menu;
    }
};
class GCodeViewerTaskBarIcon : public wxTaskBarIcon
{
public:
    GCodeViewerTaskBarIcon(wxTaskBarIconType iconType = wxTBI_DEFAULT_TYPE) : wxTaskBarIcon(iconType) {}
    wxMenu *CreatePopupMenu() override {
        wxMenu *menu = new wxMenu;
        append_menu_item(menu, wxID_ANY, _L("Open Slic3r"), _L("Open a new Slic3r instance"),
            [this](wxCommandEvent&) { start_new_slicer(nullptr, true); }, "", nullptr);
        append_menu_item(menu, wxID_ANY, _L("G-code preview") + dots, _L("Open new G-code viewer"),
            [this](wxCommandEvent&) { start_new_gcodeviewer_open_file(); }, "", nullptr);
        return menu;
    }
};
#endif // __APPLE__

// Load the icon either from the exe, or from the ico file.
static wxIcon main_frame_icon(GUI_App::EAppMode app_mode)
{
#if _WIN32
    std::wstring path(size_t(MAX_PATH), wchar_t(0));
    int len = int(::GetModuleFileName(nullptr, path.data(), MAX_PATH));
    if (len > 0 && len < MAX_PATH) {
        path.erase(path.begin() + len, path.end());
        if (app_mode == GUI_App::EAppMode::GCodeViewer) {
            // Only in case the slicer was started with --gcodeviewer parameter try to load the icon from gcodeviewer.exe
            // Otherwise load it from the exe.
            for (const std::wstring_view exe_name : { std::wstring_view(SLIC3R_APP_WCMD L".exe"), std::wstring_view(SLIC3R_APP_WCMD L"_console.exe") })
                if (boost::iends_with(path, exe_name)) {
                    path.erase(path.end() - exe_name.size(), path.end());
                    path += GCODEVIEWER_APP_WCMD L".exe";
                    break;
                }
        }
    }
    return wxIcon(path, wxBITMAP_TYPE_ICO);
#else // _WIN32
    return wxIcon(Slic3r::var(app_mode == GUI_App::EAppMode::Editor ? SLIC3R_APP_KEY "_128px.png" : GCODEVIEWER_APP_KEY "_128px.png"), wxBITMAP_TYPE_PNG);
#endif // _WIN32
}

MainFrame::MainFrame() :
DPIFrame(NULL, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE, "mainframe"),
    m_printhost_queue_dlg(new PrintHostQueueDialog(this))
    , m_recent_projects(9)
    , m_settings_dialog(this)
{
    // Fonts were created by the DPIFrame constructor for the monitor, on which the window opened.
    wxGetApp().update_fonts(this);
/*
#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList 
    this->SetFont(this->normal_font());
#endif
    // Font is already set in DPIFrame constructor
*/

#ifdef __APPLE__
    // Initialize the docker task bar icon.
    switch (wxGetApp().get_app_mode()) {
    default:
    case GUI_App::EAppMode::Editor:
        m_taskbar_icon = std::make_unique<PrusaSlicerTaskBarIcon>(wxTBI_DOCK);
        m_taskbar_icon->SetIcon(wxIcon(Slic3r::var(SLIC3R_APP_KEY "_128px.png"), wxBITMAP_TYPE_PNG), SLIC3R_APP_KEY);
        break;
    case GUI_App::EAppMode::GCodeViewer:
        m_taskbar_icon = std::make_unique<GCodeViewerTaskBarIcon>(wxTBI_DOCK);
        m_taskbar_icon->SetIcon(wxIcon(Slic3r::var(GCODEVIEWER_APP_KEY "_128px.png"), wxBITMAP_TYPE_PNG), GCODEVIEWER_APP_NAME);
        break;
    }
#endif // __APPLE__

    // Load the icon either from the exe, or from the ico file.
    SetIcon(main_frame_icon(wxGetApp().get_app_mode()));

	// initialize status bar
	m_statusbar = std::make_shared<ProgressStatusBar>(this);
    m_statusbar->set_font(GUI::wxGetApp().normal_font());
    if (wxGetApp().is_editor())
	m_statusbar->embed(this);
    m_statusbar->set_status_text(_L("Version") + " " +
        SLIC3R_VERSION + " " +
        _L("Remember to check for updates at") + " " + SLIC3R_DOWNLOAD);

    // initialize tabpanel and menubar
    init_tabpanel();
    if (wxGetApp().is_gcode_viewer())
        init_menubar_as_gcodeviewer();
    else
        init_menubar_as_editor();

#if _WIN32
    // This is needed on Windows to fake the CTRL+# of the window menu when using the numpad
    wxAcceleratorEntry entries[6];
    entries[0].Set(wxACCEL_CTRL, WXK_NUMPAD1, wxID_HIGHEST + 1);
    entries[1].Set(wxACCEL_CTRL, WXK_NUMPAD2, wxID_HIGHEST + 2);
    entries[2].Set(wxACCEL_CTRL, WXK_NUMPAD3, wxID_HIGHEST + 3);
    entries[3].Set(wxACCEL_CTRL, WXK_NUMPAD4, wxID_HIGHEST + 4);
    entries[4].Set(wxACCEL_CTRL, WXK_NUMPAD5, wxID_HIGHEST + 5);
    entries[5].Set(wxACCEL_CTRL, WXK_NUMPAD6, wxID_HIGHEST + 6);
    wxAcceleratorTable accel(6, entries);
    SetAcceleratorTable(accel);
#endif // _WIN32

    // set default tooltip timer in msec
    // SetAutoPop supposedly accepts long integers but some bug doesn't allow for larger values
    // (SetAutoPop is not available on GTK.)
    wxToolTip::SetAutoPop(32767);

    m_loaded = true;

    // initialize layout
    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    wxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_main_sizer, 1, wxEXPAND);
    SetSizer(sizer);
    // initialize layout from config
        update_layout();
    sizer->SetSizeHints(this);
    Fit();

    const wxSize min_size = wxGetApp().get_min_size(); //wxSize(76*wxGetApp().em_unit(), 49*wxGetApp().em_unit());
#ifdef __APPLE__
    // Using SetMinSize() on Mac messes up the window position in some cases
    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
    SetSize(min_size/*wxSize(760, 490)*/);
#else
    SetMinSize(min_size/*wxSize(760, 490)*/);
    SetSize(GetMinSize());
#endif
    Layout();

    update_title();

    // declare events
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        if (event.CanVeto() && !wxGetApp().check_unsaved_changes()) {
            event.Veto();
            return;
        }
        if (event.CanVeto() && plater() && !plater()->check_project_unsaved_changes()) {
            event.Veto();
            return;
        }
        if (event.CanVeto() && !wxGetApp().check_print_host_queue()) {
            event.Veto();
            return;
        }
        this->shutdown();
        // propagate event
        event.Skip();
    });

    //FIXME it seems this method is not called on application start-up, at least not on Windows. Why?
    // The same applies to wxEVT_CREATE, it is not being called on startup on Windows.
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (m_plater != nullptr && event.GetActive())
            m_plater->on_activate();
        event.Skip();
    });

// OSX specific issue:
// When we move application between Retina and non-Retina displays, The legend on a canvas doesn't redraw
// So, redraw explicitly canvas, when application is moved
//FIXME maybe this is useful for __WXGTK3__ as well?
#if __APPLE__
    Bind(wxEVT_MOVE, [this](wxMoveEvent& event) {
        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
        wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
        event.Skip();
    });
#endif

    wxGetApp().persist_window_geometry(this, true);
    wxGetApp().persist_window_geometry(&m_settings_dialog, true);

    update_ui_from_settings();    // FIXME (?)

    if (m_plater != nullptr) {
        m_plater->get_collapse_toolbar().set_enabled(wxGetApp().app_config->get("show_collapse_button") == "1");
        m_plater->show_action_buttons(true);
    }
}

void MainFrame::update_layout()
{
    auto restore_to_creation = [this]() {
        auto clean_sizer = [](wxSizer* sizer) {
            while (!sizer->GetChildren().IsEmpty()) {
                sizer->Detach(0);
            }
        };

        // On Linux m_plater needs to be removed from m_tabpanel before to reparent it
        //clear if previous was old
        m_tabpanel_stop_event = true;
        int plater_page_id = m_tabpanel->FindPage(m_plater);
        if (plater_page_id != wxNOT_FOUND)
            m_tabpanel->RemovePage(plater_page_id);

        if (m_plater->GetParent() != this)
            m_plater->Reparent(this);

        for (int i = 0; i < m_tabpanel->GetPageCount();  i++) {
            m_tabpanel->SetPageImage(i, -1);
        }
        m_tabpanel->SetImageList(nullptr); //clear
         

        if (m_tabpanel->GetParent() != this)
            m_tabpanel->Reparent(this);

        //clear if previous was hidden
        plater_page_id = (m_plater_page != nullptr) ? m_tabpanel->FindPage(m_plater_page) : wxNOT_FOUND;
        if (plater_page_id != wxNOT_FOUND) {
            m_tabpanel->DeletePage(plater_page_id);
            m_plater_page = nullptr;
        }

        //clear if previous was tabs
        for (int i = 0; i < m_tabpanel->GetPageCount() - 3; i++)
            if (m_tabpanel->GetPage(i)->GetChildren().empty() && m_tabpanel->GetPage(i)->GetSizer()->GetItemCount() > 0) {
                clean_sizer(m_tabpanel->GetPage(i)->GetSizer());
            }
        if (m_tabpanel->GetPageCount() >= 6 && m_tabpanel->GetPage(0)->GetChildren().size() == 0 && m_tabpanel->GetPage(1)->GetChildren().size() == 0 && m_tabpanel->GetPage(2)->GetChildren().size() == 0) {
            m_tabpanel->DeletePage(2);
            m_tabpanel->DeletePage(1);
            m_tabpanel->DeletePage(0);
        }
        // ensure wehave only the 3 settings tabs
        while (m_tabpanel->GetPageCount() > 3) {
            m_tabpanel->DeletePage(0);
        }

        clean_sizer(m_main_sizer);
        clean_sizer(m_settings_dialog.GetSizer());

        if (m_settings_dialog.IsShown())
            m_settings_dialog.Close();

        m_tabpanel->Hide();
        m_tabpanel_stop_event = false;
        m_plater->Hide();
        m_plater->enable_view_toolbar(true);
        m_plater->set_force_preview(Preview::ForceState::NoForce);

        Layout();
    };

    ESettingsLayout layout = wxGetApp().is_gcode_viewer() ? ESettingsLayout::GCodeViewer :
           (wxGetApp().app_config->get("old_settings_layout_mode") == "1" ? ESettingsLayout::Old :
            wxGetApp().app_config->get("tab_settings_layout_mode") == "1" ? ESettingsLayout::Tabs :
            wxGetApp().app_config->get("new_settings_layout_mode") == "1" ? ESettingsLayout::Hidden :
            wxGetApp().app_config->get("dlg_settings_layout_mode") == "1" ? ESettingsLayout::Dlg :
#ifdef __WXMSW__
                ESettingsLayout::Tabs);
#else
                ESettingsLayout::Old);
#endif

    if (m_layout == layout)
        return;

    wxBusyCursor busy;

    Freeze();

    // Remove old settings
    if (m_layout != ESettingsLayout::Unknown)
        restore_to_creation();
    else //init with view_toolbar by default
        m_plater->enable_view_toolbar(true);

#ifdef __WXMSW__
    enum class State {
        noUpdate,
        fromDlg,
        toDlg
    };
    State update_scaling_state = //m_layout == ESettingsLayout::Unknown   ? State::noUpdate   : // don't scale settings dialog from the application start
                                 m_layout == ESettingsLayout::Dlg       ? State::fromDlg    :
                                 layout   == ESettingsLayout::Dlg       ? State::toDlg      : State::noUpdate;
#endif //__WXMSW__

    m_layout = layout;
    if (m_plater && m_layerpreview_menu_item)
        m_layerpreview_menu_item->Enable(m_layout == ESettingsLayout::Tabs || m_layout == ESettingsLayout::Old);

    // From the very beginning the Print settings should be selected
    m_last_selected_setting_tab = 0;
    m_last_selected_plater_tab = 999;

    // Set new settings
    switch (m_layout)
    {
    case ESettingsLayout::Unknown:
    {
        break;
    }case ESettingsLayout::Old:
    {
        //layout
        m_plater->Reparent(m_tabpanel);
        m_tabpanel->InsertPage(0, m_plater, _L("Plater"));
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND);
        // icons for ESettingsLayout::Old
        wxImageList* img_list = nullptr;
        int icon_size = 0;
        try {
            icon_size = atoi(wxGetApp().app_config->get("tab_icon_size").c_str());
        }
        catch (std::exception e) {}
        if (m_tabpanel->GetPageCount() == 4 && icon_size >= 8) {
            std::initializer_list<std::string> icon_list = { "plater", "cog", "spool_cog", "printer_cog" };
            if (icon_size < 16)
                icon_list = { "plater", "cog", "spool", "printer" };
            for (std::string icon_name : icon_list) {
                const wxBitmap& bmp = create_scaled_bitmap(icon_name, this, icon_size);
                if (img_list == nullptr)
                    img_list = new wxImageList(bmp.GetWidth(), bmp.GetHeight());
                img_list->Add(bmp);
            }
            m_tabpanel->AssignImageList(img_list);
            m_tabpanel->SetPageImage(0, 0);
            m_tabpanel->SetPageImage(1, 1);
            m_tabpanel->SetPageImage(2, 2);
            m_tabpanel->SetPageImage(3, 3);
        }
        // show
        m_plater->Show();
        m_tabpanel->Show();
        break;
    }
    case ESettingsLayout::Tabs:
    {
        // don't use view_toolbar here
        m_plater->enable_view_toolbar(false);
        bool need_freeze = !this->IsFrozen();
        if(need_freeze) this->Freeze();
        // icons for ESettingsLayout::Tabs
        wxImageList* img_list = nullptr;
        int icon_size = 0;
        try {
            icon_size = atoi(wxGetApp().app_config->get("tab_icon_size").c_str());
        }
        catch (std::exception e) {}
        if (icon_size >= 8) {
            std::initializer_list<std::string> icon_list = { "editor_menu", "layers", "preview_menu", "cog", "spool_cog", "printer_cog" };
            if (icon_size < 16)
                icon_list = { "editor_menu", "layers", "preview_menu", "cog", "spool", "printer" };
            for (std::string icon_name : icon_list) {
                const wxBitmap& bmp = create_scaled_bitmap(icon_name, this, icon_size);
                if (img_list == nullptr)
                    img_list = new wxImageList(bmp.GetWidth(), bmp.GetHeight());
                img_list->Add(bmp);
            }
        }
        wxPanel* first_panel = new wxPanel(m_tabpanel);
        m_tabpanel->InsertPage(0, first_panel, _L("3D view"));
        m_tabpanel->InsertPage(1, new wxPanel(m_tabpanel), _L("Sliced preview"));
        m_tabpanel->InsertPage(2, new wxPanel(m_tabpanel), _L("Gcode preview"));
        if (m_tabpanel->GetPageCount() == 6) {
            m_tabpanel->AssignImageList(img_list);
            m_tabpanel->GetPage(0)->SetSizer(new wxBoxSizer(wxVERTICAL));
            m_tabpanel->GetPage(1)->SetSizer(new wxBoxSizer(wxVERTICAL));
            m_tabpanel->GetPage(2)->SetSizer(new wxBoxSizer(wxVERTICAL));
            if (icon_size >= 8)
            {
                m_tabpanel->SetPageImage(0, 0);
                m_tabpanel->SetPageImage(1, 1);
                m_tabpanel->SetPageImage(2, 2);
                m_tabpanel->SetPageImage(3, 3);
                m_tabpanel->SetPageImage(4, 4);
                m_tabpanel->SetPageImage(5, 5);
            }
        }
        m_plater->Reparent(first_panel);
        first_panel->GetSizer()->Add(m_plater, 1, wxEXPAND);
        m_tabpanel->ChangeSelection(0);
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND);
        m_plater->Show();
        m_tabpanel->Show();
        if (need_freeze) this->Thaw();
        break;
    }
    case ESettingsLayout::Hidden:
    {
        m_main_sizer->Add(m_plater, 1, wxEXPAND);
        m_tabpanel->Hide();
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND);
        m_plater_page = new wxPanel(m_tabpanel);
        m_tabpanel->InsertPage(0, m_plater_page, _L("Plater")); // empty panel just for Plater tab */
        m_plater->Show();
        break;
    }
    case ESettingsLayout::Dlg:
    {
        m_main_sizer->Add(m_plater, 1, wxEXPAND);
        m_tabpanel->Reparent(&m_settings_dialog);
        m_settings_dialog.GetSizer()->Add(m_tabpanel, 1, wxEXPAND);
        m_tabpanel->Show();
        m_plater->Show();
        break;
    }
    case ESettingsLayout::GCodeViewer:
    {
        m_main_sizer->Add(m_plater, 1, wxEXPAND);
        m_plater->set_bed_shape({ { 0.0, 0.0 }, { 200.0, 0.0 }, { 200.0, 200.0 }, { 0.0, 200.0 } }, "", "", true);
        m_plater->get_collapse_toolbar().set_enabled(false);
        m_plater->collapse_sidebar(true);
        m_plater->Show();
        break;
    }
    }

#ifdef __WXMSW__
    if (update_scaling_state != State::noUpdate)
    {
        int mainframe_dpi   = get_dpi_for_window(this);
        int dialog_dpi      = get_dpi_for_window(&m_settings_dialog);
        if (mainframe_dpi != dialog_dpi) {
            wxSize oldDPI = update_scaling_state == State::fromDlg ? wxSize(dialog_dpi, dialog_dpi) : wxSize(mainframe_dpi, mainframe_dpi);
            wxSize newDPI = update_scaling_state == State::toDlg   ? wxSize(dialog_dpi, dialog_dpi) : wxSize(mainframe_dpi, mainframe_dpi);

            if (update_scaling_state == State::fromDlg)
                this->enable_force_rescale();
            else
                (&m_settings_dialog)->enable_force_rescale();

            wxWindow* win { nullptr };
            if (update_scaling_state == State::fromDlg)
                win = this;
            else
                win = &m_settings_dialog;

#if wxVERSION_EQUAL_OR_GREATER_THAN(3,1,3)
            m_tabpanel->MSWUpdateOnDPIChange(oldDPI, newDPI);
            win->GetEventHandler()->AddPendingEvent(wxDPIChangedEvent(oldDPI, newDPI));
#else
            win->GetEventHandler()->AddPendingEvent(DpiChangedEvent(EVT_DPI_CHANGED_SLICER, newDPI, win->GetRect()));
#endif // wxVERSION_EQUAL_OR_GREATER_THAN
        }
    }
#endif //__WXMSW__

//#ifdef __APPLE__
//    // Using SetMinSize() on Mac messes up the window position in some cases
//    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
//    // So, if we haven't possibility to set MinSize() for the MainFrame, 
//    // set the MinSize() as a half of regular  for the m_plater and m_tabpanel, when settings layout is in slNew mode
//    // Otherwise, MainFrame will be maximized by height
//    if (m_layout == ESettingsLayout::Hidden) {
//        wxSize size = wxGetApp().get_min_size();
//        size.SetHeight(int(0.5 * size.GetHeight()));
//        m_plater->SetMinSize(size);
//        m_tabpanel->SetMinSize(size);
//    }
//#endif
    
#ifdef __APPLE__
    m_plater->sidebar().change_top_border_for_mode_sizer(m_layout != ESettingsLayout::Tabs && m_layout != ESettingsLayout::Old);
#endif
    
    Layout();
    Thaw();
}

// Called when closing the application and when switching the application language.
void MainFrame::shutdown()
{
#ifdef _WIN32
	if (m_hDeviceNotify) {
	::UnregisterDeviceNotification(HDEVNOTIFY(m_hDeviceNotify));
	m_hDeviceNotify = nullptr;
	}
 	if (m_ulSHChangeNotifyRegister) {
        SHChangeNotifyDeregister(m_ulSHChangeNotifyRegister);
        m_ulSHChangeNotifyRegister = 0;
 	}
#endif // _WIN32

    if (m_plater != nullptr) {
        m_plater->stop_jobs();

        //close calibration dialog if opened
        wxGetApp().change_calibration_dialog(nullptr, nullptr);

        // Unbinding of wxWidgets event handling in canvases needs to be done here because on MAC,
        // when closing the application using Command+Q, a mouse event is triggered after this lambda is completed,
        // causing a crash
        m_plater->unbind_canvas_event_handlers();

        // Cleanup of canvases' volumes needs to be done here or a crash may happen on some Linux Debian flavours
        // see: https://github.com/prusa3d/PrusaSlicer/issues/3964
        m_plater->reset_canvas_volumes();
    }



    // Weird things happen as the Paint messages are floating around the windows being destructed.
    // Avoid the Paint messages by hiding the main window.
    // Also the application closes much faster without these unnecessary screen refreshes.
    // In addition, there were some crashes due to the Paint events sent to already destructed windows.
    this->Show(false);

    if (m_settings_dialog.IsShown())
        // call Close() to trigger call to lambda defined into GUI_App::persist_window_geometry()
        m_settings_dialog.Close();

    if (m_plater != nullptr) {
	// Stop the background thread (Windows and Linux).
	// Disconnect from a 3DConnextion driver (OSX).
    m_plater->get_mouse3d_controller().shutdown();
	// Store the device parameter database back to appconfig.
    m_plater->get_mouse3d_controller().save_config(*wxGetApp().app_config);
    }

    // Stop the background thread of the removable drive manager, so that no new updates will be sent to the Plater.
    wxGetApp().removable_drive_manager()->shutdown();
	//stop listening for messages from other instances
	wxGetApp().other_instance_message_handler()->shutdown(this);
    // Save the slic3r.ini.Usually the ini file is saved from "on idle" callback,
    // but in rare cases it may not have been called yet.
    wxGetApp().app_config->save();
//         if (m_plater)
//             m_plater->print = undef;
//         Slic3r::GUI::deregister_on_request_update_callback();

    // set to null tabs and a plater
    // to avoid any manipulations with them from App->wxEVT_IDLE after of the mainframe closing 
    wxGetApp().tabs_list.clear();
    wxGetApp().plater_ = nullptr;
}

void MainFrame::update_title()
{
    wxString title = wxEmptyString;
    bool has_name = false;
    if (m_plater != nullptr) {
        // m_plater->get_project_filename() produces file name including path, but excluding extension.
        // Don't try to remove the extension, it would remove part of the file name after the last dot!
        wxString project = from_path(into_path(m_plater->get_project_filename()).filename());
        if (project.empty()) {
            project = m_plater->get_project_filename();
        }
        if (!project.empty()) {
            has_name = true;
            title += (project + " - ");
        }
    }

    std::string build_id = wxGetApp().is_editor() ? SLIC3R_BUILD_ID : GCODEVIEWER_BUILD_ID;
    size_t 		idx_plus = build_id.find('+');
    if (idx_plus != build_id.npos) {
    	// Parse what is behind the '+'. If there is a number, then it is a build number after the label, and full build ID is shown.
    	int commit_after_label;
    	if (! boost::starts_with(build_id.data() + idx_plus + 1, "UNKNOWN") && 
            (build_id.at(idx_plus + 1) == '-' || sscanf(build_id.data() + idx_plus + 1, "%d-", &commit_after_label) == 0)) {
    		// It is a release build.
    		build_id.erase(build_id.begin() + idx_plus, build_id.end());    		
#if defined(_WIN32) && ! defined(_WIN64)
    		// People are using 32bit slicer on a 64bit machine by mistake. Make it explicit.
            build_id += " 32 bit";
#endif
    	}
    }

    title += wxString(SLIC3R_APP_NAME) + "_" + wxString(SLIC3R_VERSION) ;
    if (wxGetApp().is_editor() && !has_name)
        title += (" " + _L(SLIC3R_BASED_ON));

    SetTitle(title);
}

void MainFrame::init_tabpanel()
{
    // wxNB_NOPAGETHEME: Disable Windows Vista theme for the Notebook background. The theme performance is terrible on Windows 10
    // with multiple high resolution displays connected.
    m_tabpanel = new wxNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
    m_tabpanel->SetFont(Slic3r::GUI::wxGetApp().normal_font());
#endif
    m_tabpanel->Hide();
    m_settings_dialog.set_tabpanel(m_tabpanel);


    m_tabpanel->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxEvent&) {
        if (m_tabpanel_stop_event)
            return;
        wxWindow* panel = m_tabpanel->GetCurrentPage();
        Tab* tab = dynamic_cast<Tab*>(panel);

        // There shouldn't be a case, when we try to select a tab, which doesn't support a printer technology
        if (panel == nullptr || (tab != nullptr && !tab->supports_printer_technology(m_plater->printer_technology())))
            return;

        std::vector<Tab*>& tabs_list = wxGetApp().tabs_list;
        int last_selected_plater_tab = m_last_selected_plater_tab;
        int last_selected_setting_tab = m_last_selected_setting_tab;
        if (tab && std::find(tabs_list.begin(), tabs_list.end(), tab) != tabs_list.end()) {
            // On GTK, the wxEVT_NOTEBOOK_PAGE_CHANGED event is triggered
            // before the MainFrame is fully set up.
            tab->OnActivate();
            if (this->m_layout == ESettingsLayout::Tabs)
                last_selected_setting_tab = m_tabpanel->GetSelection() - 3;
            if (this->m_layout == ESettingsLayout::Dlg)
                last_selected_setting_tab = m_tabpanel->GetSelection();
            else
                last_selected_setting_tab = m_tabpanel->GetSelection() - 1;
        }
        else if (this->m_layout == ESettingsLayout::Tabs) {
            if (last_selected_plater_tab == m_tabpanel->GetSelection()) {
#ifdef __APPLE__
                BOOST_LOG_TRIVIAL(debug) << "Page changed to the same one (" << m_last_selected_plater_tab << ") no need to do anything\n";
#endif
                return;
            }
            bool need_freeze = !this->IsFrozen();
            bool need_freeze_plater = false;
            if(need_freeze) Freeze();
            else {
                need_freeze_plater = !m_plater->IsFrozen();
                if (need_freeze_plater) m_plater->Freeze();
            }
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << "I switched to tab  " << m_tabpanel->GetSelection() << " and so i need to change the panel position & content\n";
#endif
            size_t new_tab = m_tabpanel->GetSelection();

            size_t max = 0;
            for (int i = 0; i < 3; i++)
                max = std::max(max, m_tabpanel->GetPage(i)->GetSizer()->GetItemCount());
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << " 1 - hide & clear the sizers: " << max << "->";
#endif
            for(int i=0;i<3;i++)
                m_tabpanel->GetPage(i)->GetSizer()->Clear();
            max = 0;
            for (int i = 0; i < 3; i++)
                max = std::max(max, m_tabpanel->GetPage(i)->GetSizer()->GetItemCount());
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << max << "\n";
#endif

            m_plater->Reparent(m_tabpanel->GetCurrentPage());
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << " 2 - change parent from tab " << m_last_selected_plater_tab << " to tab " << m_tabpanel->GetSelection() << "\n";
#endif
            if (m_tabpanel->GetSelection() == 0)
                this->m_plater->select_view_3D("3D");
            else if (m_tabpanel->GetSelection() == 1) {
                if (this->m_plater->get_force_preview() != Preview::ForceState::ForceExtrusions) {
                    this->m_plater->set_force_preview(Preview::ForceState::ForceExtrusions);
                    this->m_plater->select_view_3D("Preview");
                    this->m_plater->refresh_print();
                }else
                    this->m_plater->select_view_3D("Preview");
            }
            else if (m_tabpanel->GetSelection() == 2) {
                if (this->m_plater->get_force_preview() != Preview::ForceState::ForceGcode) {
                    this->m_plater->set_force_preview(Preview::ForceState::ForceGcode);
                    this->m_plater->select_view_3D("Preview");
                    this->m_plater->refresh_print();
                }else
                    this->m_plater->select_view_3D("Preview");
            }
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << " 3 - redraw\n";
            BOOST_LOG_TRIVIAL(debug) << " 4 - add to new sizer: " << m_tabpanel->GetCurrentPage()->GetSizer()->GetItemCount() << "->";
#endif
            m_tabpanel->GetCurrentPage()->GetSizer()->Add(m_plater, 1, wxEXPAND);
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << m_tabpanel->GetCurrentPage()->GetSizer()->GetItemCount() << "\n";
#endif
            m_plater->Show();
#ifdef __APPLE__
            BOOST_LOG_TRIVIAL(debug) << "End of change for the panel position & content, tab is "<< m_tabpanel->GetSelection() <<"\n";
#endif
            m_last_selected_plater_tab = m_tabpanel->GetSelection();

            if (need_freeze) Thaw();
            else if (need_freeze_plater) m_plater->Freeze();
#ifdef __APPLE__
            m_tabpanel->ChangeSelection(new_tab);
            m_tabpanel->Refresh();
            BOOST_LOG_TRIVIAL(debug) << "Macos: force tab selection to  "<< new_tab <<" : " << m_tabpanel->GetSelection() << "\n";
#endif
        } else {
            select_tab(MainFrame::ETabType::LastPlater); // select Plater
            m_last_selected_plater_tab = 999;
        }
    });

    m_plater = new Plater(this, this);
    m_plater->Hide();

    wxGetApp().plater_ = m_plater;

    wxGetApp().obj_list()->create_popup_menus();

    if (wxGetApp().is_editor())
        create_preset_tabs();

    if (m_plater) {
        // load initial config
        auto full_config = wxGetApp().preset_bundle->full_config();
        m_plater->on_config_change(full_config);

        // Show a correct number of filament fields.
        // nozzle_diameter is undefined when SLA printer is selected
        if (full_config.has("nozzle_diameter")) {
            m_plater->on_extruders_change(full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size());
        }
    }
}

#ifdef WIN32
void MainFrame::register_win32_callbacks()
{
    //static GUID GUID_DEVINTERFACE_USB_DEVICE  = { 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED };
    //static GUID GUID_DEVINTERFACE_DISK        = { 0x53f56307, 0xb6bf, 0x11d0, 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b };
    //static GUID GUID_DEVINTERFACE_VOLUME      = { 0x71a27cdd, 0x812a, 0x11d0, 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f };
    static GUID GUID_DEVINTERFACE_HID           = { 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 };

    // Register USB HID (Human Interface Devices) notifications to trigger the 3DConnexion enumeration.
    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter = { 0 };
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    NotificationFilter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    m_hDeviceNotify = ::RegisterDeviceNotification(this->GetHWND(), &NotificationFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

// or register for file handle change?
//      DEV_BROADCAST_HANDLE NotificationFilter = { 0 };
//      NotificationFilter.dbch_size = sizeof(DEV_BROADCAST_HANDLE);
//      NotificationFilter.dbch_devicetype = DBT_DEVTYP_HANDLE;

    // Using Win32 Shell API to register for media insert / removal events.
    LPITEMIDLIST ppidl;
    if (SHGetSpecialFolderLocation(this->GetHWND(), CSIDL_DESKTOP, &ppidl) == NOERROR) {
        SHChangeNotifyEntry shCNE;
        shCNE.pidl       = ppidl;
        shCNE.fRecursive = TRUE;
        // Returns a positive integer registration identifier (ID).
        // Returns zero if out of memory or in response to invalid parameters.
        m_ulSHChangeNotifyRegister = SHChangeNotifyRegister(this->GetHWND(),        // Hwnd to receive notification
            SHCNE_DISKEVENTS,                                                       // Event types of interest (sources)
            SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED,
            //SHCNE_UPDATEITEM,                                                     // Events of interest - use SHCNE_ALLEVENTS for all events
            WM_USER_MEDIACHANGED,                                                   // Notification message to be sent upon the event
            1,                                                                      // Number of entries in the pfsne array
            &shCNE);                                                                // Array of SHChangeNotifyEntry structures that 
                                                                                    // contain the notifications. This array should 
                                                                                    // always be set to one when calling SHChnageNotifyRegister
                                                                                    // or SHChangeNotifyDeregister will not work properly.
        assert(m_ulSHChangeNotifyRegister != 0);    // Shell notification failed
    } else {
        // Failed to get desktop location
        assert(false); 
    }

    {
        static constexpr int device_count = 1;
        RAWINPUTDEVICE devices[device_count] = { 0 };
        // multi-axis mouse (SpaceNavigator, etc.)
        devices[0].usUsagePage = 0x01;
        devices[0].usUsage = 0x08;
        if (! RegisterRawInputDevices(devices, device_count, sizeof(RAWINPUTDEVICE)))
            BOOST_LOG_TRIVIAL(error) << "RegisterRawInputDevices failed";
    }
}
#endif // _WIN32

void MainFrame::create_preset_tabs()
{
    wxGetApp().update_label_colours_from_appconfig();
    add_created_tab(new TabPrint(m_tabpanel));
    add_created_tab(new TabFilament(m_tabpanel));
    add_created_tab(new TabSLAPrint(m_tabpanel));
    add_created_tab(new TabSLAMaterial(m_tabpanel));
    add_created_tab(new TabPrinter(m_tabpanel));
}

void MainFrame::add_created_tab(Tab* panel)
{
    panel->create_preset_tab();

    const auto printer_tech = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();

    if (panel->supports_printer_technology(printer_tech))
        m_tabpanel->AddPage(panel, panel->title());
}

bool MainFrame::is_active_and_shown_tab(Tab* tab)
{
    int page_id = m_tabpanel->FindPage(tab);

    if (m_tabpanel->GetSelection() != page_id)
        return false;

    if (m_layout == ESettingsLayout::Dlg)
        return m_settings_dialog.IsShown();

    if (m_layout == ESettingsLayout::Hidden)
        return m_main_sizer->IsShown(m_tabpanel);
    
    return true;
}

bool MainFrame::can_start_new_project() const
{
    return (m_plater != nullptr);
}

bool MainFrame::can_save() const
{
    return (m_plater != nullptr);
}

bool MainFrame::can_export_model() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

bool MainFrame::can_export_toolpaths() const
{
    return (m_plater != nullptr) && (m_plater->printer_technology() == ptFFF) && m_plater->is_preview_shown() && m_plater->is_preview_loaded() && m_plater->has_toolpaths_to_export();
}

bool MainFrame::can_export_supports() const
{
    if ((m_plater == nullptr) || (m_plater->printer_technology() != ptSLA) || m_plater->model().objects.empty())
        return false;

    bool can_export = false;
    const PrintObjects& objects = m_plater->sla_print().objects();
    for (const SLAPrintObject* object : objects)
    {
        if (object->has_mesh(slaposPad) || object->has_mesh(slaposSupportTree))
        {
            can_export = true;
            break;
        }
    }
    return can_export;
}

bool MainFrame::can_export_gcode() const
{
    if (m_plater == nullptr)
        return false;

    if (m_plater->model().objects.empty())
        return false;

    if (m_plater->is_export_gcode_scheduled())
        return false;

    // TODO:: add other filters

    return true;
}

bool MainFrame::can_send_gcode() const
{
    if (m_plater && ! m_plater->model().objects.empty())
        if (const DynamicPrintConfig *cfg = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config(); cfg)
            if (const auto *print_host_opt = cfg->option<ConfigOptionString>("print_host"); print_host_opt)
                return ! print_host_opt->value.empty();
    return false;
}

bool MainFrame::can_export_gcode_sd() const
{
	if (m_plater == nullptr)
		return false;

	if (m_plater->model().objects.empty())
		return false;

	if (m_plater->is_export_gcode_scheduled())
		return false;

	// TODO:: add other filters

	return wxGetApp().removable_drive_manager()->status().has_removable_drives;
}

bool MainFrame::can_eject() const
{
	return wxGetApp().removable_drive_manager()->status().has_eject;
}

bool MainFrame::can_slice() const
{
    bool bg_proc = wxGetApp().app_config->get("background_processing") == "1";
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() && !bg_proc : false;
}

bool MainFrame::can_change_view() const
{
    switch (m_layout)
    {
    default:                   { return false; }
    case ESettingsLayout::Hidden: { return m_plater->IsShown(); }
    case ESettingsLayout::Dlg: { return true; }
    case ESettingsLayout::Old: 
    case ESettingsLayout::Tabs: { 
        int page_id = m_tabpanel->GetSelection();
        return page_id != wxNOT_FOUND && selected_tab() <= ETabType::LastPlater;
    }
    case ESettingsLayout::GCodeViewer: { return true; }
    }
}

bool MainFrame::can_select() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

bool MainFrame::can_deselect() const
{
    return (m_plater != nullptr) && !m_plater->is_selection_empty();
}

bool MainFrame::can_delete() const
{
    return (m_plater != nullptr) && !m_plater->is_selection_empty();
}

bool MainFrame::can_delete_all() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

bool MainFrame::can_reslice() const
{
    return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

void MainFrame::on_dpi_changed(const wxRect& suggested_rect)
{
#if ENABLE_WX_3_1_3_DPI_CHANGED_EVENT
    wxGetApp().update_fonts(this);
#else
    wxGetApp().update_fonts();
#endif // ENABLE_WX_3_1_3_DPI_CHANGED_EVENT
    this->SetFont(this->normal_font());

    // update Plater
    wxGetApp().plater()->msw_rescale();

    // update Tabs
    if (m_layout != ESettingsLayout::Dlg) // Do not update tabs if the Settings are in the separated dialog
    for (auto tab : wxGetApp().tabs_list)
        tab->msw_rescale();

    wxMenuBar* menu_bar = this->GetMenuBar();
    for (size_t id = 0; id < menu_bar->GetMenuCount(); id++)
        msw_rescale_menu(menu_bar->GetMenu(id));

    // Workarounds for correct Window rendering after rescale

    /* Even if Window is maximized during moving,
     * first of all we should imitate Window resizing. So:
     * 1. cancel maximization, if it was set
     * 2. imitate resizing
     * 3. set maximization, if it was set
     */
    const bool is_maximized = this->IsMaximized();
    if (is_maximized)
        this->Maximize(false);

    /* To correct window rendering (especially redraw of a status bar)
     * we should imitate window resizing.
     */
    const wxSize& sz = this->GetSize();
    this->SetSize(sz.x + 1, sz.y + 1);
    this->SetSize(sz);

    this->Maximize(is_maximized);
}

void MainFrame::on_sys_color_changed()
{
    wxBusyCursor wait;

    // update label colors in respect to the system mode
    wxGetApp().init_label_colours();

    // update Plater
    wxGetApp().plater()->sys_color_changed();

    // update Tabs
    for (auto tab : wxGetApp().tabs_list)
        tab->sys_color_changed();

    // msw_rescale_menu updates just icons, so use it
    wxMenuBar* menu_bar = this->GetMenuBar();
    for (size_t id = 0; id < menu_bar->GetMenuCount(); id++)
        msw_rescale_menu(menu_bar->GetMenu(id));
}

#ifdef _MSC_VER
    // \xA0 is a non-breaking space. It is entered here to spoil the automatic accelerators,
    // as the simple numeric accelerators spoil all numeric data entry.
static const wxString sep = "\t\xA0";
static const wxString sep_space = "\xA0";
#else
static const wxString sep = " - ";
static const wxString sep_space = "";
#endif

static wxMenu* generate_help_menu()
{
    wxMenu* helpMenu = new wxMenu();
    append_menu_item(helpMenu, wxID_ANY, _L(SLIC3R_APP_NAME " Releases"), _L("Open the " SLIC3R_APP_NAME " releases page in your browser"),
        [](wxCommandEvent&) { wxLaunchDefaultBrowser(SLIC3R_DOWNLOAD); });
    append_menu_item(helpMenu, wxID_ANY, _L(SLIC3R_APP_NAME " wiki"), _L("Open the " SLIC3R_APP_NAME " wiki in your browser"),
        [](wxCommandEvent&) { wxLaunchDefaultBrowser("http://github.com/" SLIC3R_GITHUB "/wiki"); });
    append_menu_item(helpMenu, wxID_ANY, _L(SLIC3R_APP_NAME " website"), _L("Open the Slic3r website in your browser"),
        [](wxCommandEvent&) { wxLaunchDefaultBrowser("http://slic3r.org"); });
    //#        my $versioncheck = $self->_append_menu_item($helpMenu, "Check for &Updates...", "Check for new Slic3r versions", sub{
    //#            wxTheApp->check_version(1);
    //#        });
    //#        $versioncheck->Enable(wxTheApp->have_version_check);
    append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("Slic3r Manual")),
        wxString::Format(_L("Open the Slic3r Manual in your browser")),
        //            [this](wxCommandEvent&) { wxGetApp().open_web_page_localized("http://manual.slic3r.org"); });
        //        append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("%s &Manual"), SLIC3R_APP_NAME),
        //                                             wxString::Format(_L("Open the %s manual in your browser"), SLIC3R_APP_NAME),
        [](wxCommandEvent&) { wxLaunchDefaultBrowser("http://manual.slic3r.org/"); });
    helpMenu->AppendSeparator();
    append_menu_item(helpMenu, wxID_ANY, _L("System &Info"), _L("Show system information"),
        [](wxCommandEvent&) { wxGetApp().system_info(); });
    append_menu_item(helpMenu, wxID_ANY, _L("Show &Configuration Folder"), _L("Show user configuration folder (datadir)"),
        [](wxCommandEvent&) { Slic3r::GUI::desktop_open_datadir_folder(); });
    append_menu_item(helpMenu, wxID_ANY, _L("Report an I&ssue"), wxString::Format(_L("Report an issue on %s"), SLIC3R_APP_NAME),
        [](wxCommandEvent&) { wxLaunchDefaultBrowser("http://github.com/" SLIC3R_GITHUB "/issues/new"); });

    if (wxGetApp().is_editor())
        append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("&About %s"), SLIC3R_APP_NAME), _L("Show about dialog"),
            [](wxCommandEvent&) { Slic3r::GUI::about(); });
    else {
        append_menu_item(helpMenu, wxID_ANY, wxString::Format(_L("&About %s"), GCODEVIEWER_APP_NAME), _L("Show about dialog"),
            [](wxCommandEvent&) { Slic3r::GUI::about(); });
        helpMenu->AppendSeparator();
    }
    append_menu_item(helpMenu, wxID_ANY, _L("Keyboard Shortcuts") + sep + "&?", _L("Show the list of the keyboard shortcuts"),
        [](wxCommandEvent&) { wxGetApp().keyboard_shortcuts(); });
#if ENABLE_THUMBNAIL_GENERATOR_DEBUG
    helpMenu->AppendSeparator();
    append_menu_item(helpMenu, wxID_ANY, "DEBUG gcode thumbnails", "DEBUG ONLY - read the selected gcode file and generates png for the contained thumbnails",
        [](wxCommandEvent&) { wxGetApp().gcode_thumbnails_debug(); });
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG

    return helpMenu;
}

static void add_common_view_menu_items(wxMenu* view_menu, MainFrame* mainFrame, std::function<bool(void)> can_change_view)
{
    // The camera control accelerators are captured by GLCanvas3D::on_char().
    append_menu_item(view_menu, wxID_ANY, _L("Iso") + sep + "&0", _L("Iso View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("iso"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    view_menu->AppendSeparator();
    //TRN To be shown in the main menu View->Top 
    append_menu_item(view_menu, wxID_ANY, _L("Top") + sep + "&1", _L("Top View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("top"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    //TRN To be shown in the main menu View->Bottom 
    append_menu_item(view_menu, wxID_ANY, _L("Bottom") + sep + "&2", _L("Bottom View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("bottom"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Front") + sep + "&3", _L("Front View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("front"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Rear") + sep + "&4", _L("Rear View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("rear"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Left") + sep + "&5", _L("Left View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("left"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L("Right") + sep + "&6", _L("Right View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("right"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
}

void MainFrame::init_menubar_as_editor()
{
#ifdef __APPLE__
    wxMenuBar::SetAutoWindowMenu(false);
#endif

    // File menu
    wxMenu* fileMenu = new wxMenu;
    {
        append_menu_item(fileMenu, wxID_ANY, _L("&New Project") + "\tCtrl+N", _L("Start a new project"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->ask_for_new_project(); }, "", nullptr,
            [this](){return m_plater != nullptr && can_start_new_project(); }, this);
        append_menu_item(fileMenu, wxID_ANY, _L("&Open Project") + dots + "\tCtrl+O", _L("Open a project file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->load_project(); }, "open", nullptr,
            [this](){return m_plater != nullptr; }, this);

        wxMenu* recent_projects_menu = new wxMenu();
        wxMenuItem* recent_projects_submenu = append_submenu(fileMenu, recent_projects_menu, wxID_ANY, _L("Recent projects"), "");
        m_recent_projects.UseMenu(recent_projects_menu);
        Bind(wxEVT_MENU, [this](wxCommandEvent& evt) {
            size_t file_id = evt.GetId() - wxID_FILE1;
            wxString filename = m_recent_projects.GetHistoryFile(file_id);
            if (wxFileExists(filename))
                m_plater->load_project(filename);
            else
            {
                wxMessageDialog msg(this, _L("The selected project is no longer available.\nDo you want to remove it from the recent projects list?"), _L("Error"), wxYES_NO | wxYES_DEFAULT);
                if (msg.ShowModal() == wxID_YES)
                {
                    m_recent_projects.RemoveFileFromHistory(file_id);
                        std::vector<std::string> recent_projects;
                        size_t count = m_recent_projects.GetCount();
                        for (size_t i = 0; i < count; ++i)
                        {
                            recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
                        }
                    wxGetApp().app_config->set_recent_projects(recent_projects);
                    wxGetApp().app_config->save();
                }
            }
            }, wxID_FILE1, wxID_FILE9);

        std::vector<std::string> recent_projects = wxGetApp().app_config->get_recent_projects();
        std::reverse(recent_projects.begin(), recent_projects.end());
        for (const std::string& project : recent_projects)
        {
            m_recent_projects.AddFileToHistory(from_u8(project));
        }

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(m_recent_projects.GetCount() > 0); }, recent_projects_submenu->GetId());

        append_menu_item(fileMenu, wxID_ANY, _L("&Save Project") + "\t" + GUI::shortkey_ctrl_prefix() + "S", _L("Save current project file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project_as_3mf(into_path(m_plater->get_project_filename(".3mf"))); }, "save", nullptr,
            [this](){return m_plater != nullptr && can_save(); }, this);
#ifdef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project &as") + dots + "\t" + GUI::shortkey_ctrl_prefix() + "Shift+S", _L("Save current project file as"),
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project &as") + dots + "\t" + GUI::shortkey_ctrl_prefix() + "Alt+S", _L("Save current project file as"),
#endif // __APPLE__
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project_as_3mf(); }, "save", nullptr,
            [this](){return m_plater != nullptr && can_save(); }, this);

        fileMenu->AppendSeparator();

        wxMenu* import_menu = new wxMenu();
        append_menu_item(import_menu, wxID_ANY, _L("Import STL/OBJ/AM&F/3MF") + dots + "\t" + GUI::shortkey_ctrl_prefix() + "I", _L("Load a model"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->add_model(); }, "import_plater", nullptr,
            [this](){return m_plater != nullptr; }, this);
        
        append_menu_item(import_menu, wxID_ANY, _L("Import STL (imperial units)"), _L("Load an model saved with imperial units"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->add_model(true); }, "import_plater", nullptr,
            [this](){return m_plater != nullptr; }, this);
        
        append_menu_item(import_menu, wxID_ANY, _L("Import SL1 archive") + dots, _L("Load an SL1 archive"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->import_sl1_archive(); }, "import_plater", nullptr,
            [this](){return m_plater != nullptr; }, this);    
    
        import_menu->AppendSeparator();
        append_menu_item(import_menu, wxID_ANY, _L("Import &Config") + dots + "\t" + GUI::shortkey_ctrl_prefix() + "L", _L("Load exported configuration file"),
            [this](wxCommandEvent&) { load_config_file(); }, "import_config", nullptr,
            []() {return true; }, this);
        append_menu_item(import_menu, wxID_ANY, _L("Import Config from &project") + dots +"\t" + GUI::shortkey_ctrl_prefix() + "Alt+L", _L("Load configuration from project file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->extract_config_from_project(); }, "import_config", nullptr,
            []() {return true; }, this);
        import_menu->AppendSeparator();
        append_menu_item(import_menu, wxID_ANY, _L("Import Config &Bundle") + dots, _L("Load presets from a bundle"),
            [this](wxCommandEvent&) { load_configbundle(); }, "import_config_bundle", nullptr,
            []() {return true; }, this);
        append_submenu(fileMenu, import_menu, wxID_ANY, _L("&Import"), "");

        wxMenu* export_menu = new wxMenu();
        wxMenuItem* item_export_gcode = append_menu_item(export_menu, wxID_ANY, _L("Export &G-code") + dots +"\t" + GUI::shortkey_ctrl_prefix() + "G", _L("Export current plate as G-code"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_gcode(false); }, "export_gcode", nullptr,
            [this](){return can_export_gcode(); }, this);
        m_changeable_menu_items.push_back(item_export_gcode);
        wxMenuItem* item_send_gcode = append_menu_item(export_menu, wxID_ANY, _L("S&end G-code") + dots +"\t" + GUI::shortkey_ctrl_prefix() + "Shift+G", _L("Send to print current plate as G-code"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->send_gcode(); }, "export_gcode", nullptr,
            [this](){return can_send_gcode(); }, this);
        m_changeable_menu_items.push_back(item_send_gcode);
		append_menu_item(export_menu, wxID_ANY, _L("Export G-code to SD card / Flash drive") + dots + "\t" + GUI::shortkey_ctrl_prefix() + "U", _L("Export current plate as G-code to SD card / Flash drive"),
			[this](wxCommandEvent&) { if (m_plater) m_plater->export_gcode(true); }, "export_to_sd", nullptr,
			[this]() {return can_export_gcode_sd(); }, this);
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _L("Export plate as &STL") + dots, _L("Export current plate as STL"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_stl(); }, "export_plater", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export plate as STL &including supports") + dots, _L("Export current plate as STL including supports"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_stl(true); }, "export_plater", nullptr,
            [this](){return can_export_supports(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export plate as &AMF") + dots, _L("Export current plate as AMF"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_amf(); }, "export_plater", nullptr,
            [this](){return can_export_model(); }, this);
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _L("Export &toolpaths as OBJ") + dots, _L("Export toolpaths as OBJ"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->export_toolpaths_to_obj(); }, "export_plater", nullptr,
            [this]() {return can_export_toolpaths(); }, this);
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _L("Export &Config") + dots +"\t" + GUI::shortkey_ctrl_prefix() + "E", _L("Export current configuration to file"),
            [this](wxCommandEvent&) { export_config(); }, "export_config", nullptr,
            []() {return true; }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export Config &Bundle") + dots, _L("Export all presets to file"),
            [this](wxCommandEvent&) { export_configbundle(); }, "export_config_bundle", nullptr,
            []() {return true; }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export Config Bundle With Physical Printers") + dots, _L("Export all presets including physical printers to file"),
            [this](wxCommandEvent&) { export_configbundle(true); }, "export_config_bundle", nullptr,
            []() {return true; }, this);
        export_menu->AppendSeparator();
        append_menu_item(export_menu, wxID_ANY, _L("Export to &Prusa Config") + dots, _L("Export current configuration to file, with only settings compatible with PrusaSlicer"),
            [this](wxCommandEvent&) { export_config(true); }, "export_config", nullptr,
            []() {return true; }, this);
        append_submenu(fileMenu, export_menu, wxID_ANY, _L("&Export"), "");

		append_menu_item(fileMenu, wxID_ANY, _L("Ejec&t SD card / Flash drive") + dots + "\t" + GUI::shortkey_ctrl_prefix() + "T", _L("Eject SD card / Flash drive after the G-code was exported to it."),
			[this](wxCommandEvent&) { if (m_plater) m_plater->eject_drive(); }, "eject_sd", nullptr,
			[this]() {return can_eject(); }, this);

        fileMenu->AppendSeparator();

#if 0
        m_menu_item_repeat = nullptr;
        append_menu_item(fileMenu, wxID_ANY, _L("Quick Slice") +dots+ "\t" + GUI::shortkey_ctrl_prefix() + "U", _L("Slice a file into a G-code"),
            [this](wxCommandEvent&) {
                wxTheApp->CallAfter([this]() {
                    quick_slice();
                    m_menu_item_repeat->Enable(is_last_input_file());
                }); }, "cog_go.png");
        append_menu_item(fileMenu, wxID_ANY, _L("Quick Slice and Save As") +dots +"\t" + GUI::shortkey_ctrl_prefix() + "Alt+U", _L("Slice a file into a G-code, save as"),
            [this](wxCommandEvent&) {
            wxTheApp->CallAfter([this]() {
                    quick_slice(qsSaveAs);
                    m_menu_item_repeat->Enable(is_last_input_file());
                }); }, "cog_go.png");
        m_menu_item_repeat = append_menu_item(fileMenu, wxID_ANY, _L("Repeat Last Quick Slice") +"\t" + GUI::shortkey_ctrl_prefix() + "Shift+U", _L("Repeat last quick slice"),
            [this](wxCommandEvent&) {
            wxTheApp->CallAfter([this]() {
                quick_slice(qsReslice);
            }); }, "cog_go.png");
        m_menu_item_repeat->Enable(false);
        fileMenu->AppendSeparator();
#endif
        m_menu_item_reslice_now = append_menu_item(fileMenu, wxID_ANY, _L("(Re)Slice No&w") + "\t" + GUI::shortkey_ctrl_prefix() + "R", _L("Start new slicing process"),
            [this](wxCommandEvent&) { reslice_now(); }, "re_slice", nullptr,
            [this]() { return m_plater != nullptr && can_reslice(); }, this);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _L("&Repair STL file") + dots, _L("Automatically repair an STL file"),
            [this](wxCommandEvent&) { repair_stl(); }, "wrench", nullptr,
            []() { return true; }, this);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _L("&G-code preview") + dots, _L("Open G-code viewer"),
            [this](wxCommandEvent&) { start_new_gcodeviewer_open_file(this); }, "", nullptr);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_EXIT, _L("&Quit"), wxString::Format(_L("Quit %s"), SLIC3R_APP_NAME),
            [this](wxCommandEvent&) { Close(false); }, "exit");
    }

    // Edit menu
    wxMenu* editMenu = nullptr;
    if (m_plater != nullptr)
    {
        editMenu = new wxMenu();
    #ifdef __APPLE__
        // Backspace sign
        wxString hotkey_delete = "\u232b";
    #else
        wxString hotkey_delete = "Del";
    #endif
        append_menu_item(editMenu, wxID_ANY, _L("&Select all") + "\t" + GUI::shortkey_ctrl_prefix() + sep_space + "A",
            _L("Selects all objects"), [this](wxCommandEvent&) { m_plater->select_all(); },
            "", nullptr, [this](){return can_select(); }, this);
        append_menu_item(editMenu, wxID_ANY, _L("D&eselect all") + "\t" + "Esc",
            _L("Deselects all objects"), [this](wxCommandEvent&) { m_plater->deselect_all(); },
            "", nullptr, [this](){return can_deselect(); }, this);
        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("&Delete selected") + "\t" + hotkey_delete,
            _L("Deletes the current selection"),[this](wxCommandEvent&) { m_plater->remove_selected(); },
            "remove_menu", nullptr, [this](){return can_delete(); }, this);
        append_menu_item(editMenu, wxID_ANY, _L("Delete &all") + "\t" + GUI::shortkey_ctrl_prefix() + sep_space + hotkey_delete,
            _L("Deletes all objects"), [this](wxCommandEvent&) { m_plater->reset_with_confirm(); },
            "delete_all_menu", nullptr, [this](){return can_delete_all(); }, this);

        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("&Undo") + "\t" + GUI::shortkey_ctrl_prefix() + sep_space + "Z",
            _L("Undo"), [this](wxCommandEvent&) { m_plater->undo(); },
            "undo_menu", nullptr, [this](){return m_plater->can_undo(); }, this);
        append_menu_item(editMenu, wxID_ANY, _L("&Redo") + "\t" + GUI::shortkey_ctrl_prefix() + sep_space + "Y",
            _L("Redo"), [this](wxCommandEvent&) { m_plater->redo(); },
            "redo_menu", nullptr, [this](){return m_plater->can_redo(); }, this);

        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("&Copy") + "\t" + GUI::shortkey_ctrl_prefix() + sep_space + "C",
            _L("Copy selection to clipboard"), [this](wxCommandEvent&) { m_plater->copy_selection_to_clipboard(); },
            "copy_menu", nullptr, [this](){return m_plater->can_copy_to_clipboard(); }, this);
        append_menu_item(editMenu, wxID_ANY, _L("&Paste") + "\t" + GUI::shortkey_ctrl_prefix() + sep_space + "V",
            _L("Paste clipboard"), [this](wxCommandEvent&) { m_plater->paste_from_clipboard(); },
            "paste_menu", nullptr, [this](){return m_plater->can_paste_from_clipboard(); }, this);
        
        editMenu->AppendSeparator();
#ifdef __APPLE__
        append_menu_item(editMenu, wxID_ANY, _L("Re&load from disk") + dots + "\tCtrl+Shift+R",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_all_from_disk(); },
            "", nullptr, [this]() {return !m_plater->model().objects.empty(); }, this);
#else
        append_menu_item(editMenu, wxID_ANY, _L("Re&load from disk") + "\t" + "F5",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_all_from_disk(); },
            "", nullptr, [this]() {return !m_plater->model().objects.empty(); }, this);
#endif // __APPLE__

        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("Searc&h") + "\tCtrl+F",
            _L("Search in settings"), [this](wxCommandEvent&) { m_plater->search(/*m_tabpanel->GetCurrentPage() == */m_plater->IsShown()); },
            "search", nullptr, []() {return true; }, this);
    }

    // Window menu
    auto windowMenu = new wxMenu();
    {
        if (m_plater) {
            append_menu_item(windowMenu, wxID_HIGHEST + 1, _L("3D &Plater Tab") + "\t" + GUI::shortkey_ctrl_prefix() + "1", _L("Show the editor of the input models"),
                [this](wxCommandEvent&) { select_tab(ETabType::Plater3D); }, "editor_menu", nullptr,
                []() {return true; }, this);
            m_layerpreview_menu_item = append_menu_item(windowMenu, wxID_HIGHEST + 2, _L("Layer previe&w Tab") + "\t" + GUI::shortkey_ctrl_prefix() + "2", _L("Show the layers from the slicing process"),
                [this](wxCommandEvent&) { select_tab(ETabType::PlaterPreview); }, "layers", nullptr,
                []() {return true; }, this);
            append_menu_item(windowMenu, wxID_HIGHEST + 3, _L("GCode Pre&view Tab") + "\t" + GUI::shortkey_ctrl_prefix() + "3", _L("Show the preview of the gcode output"),
                [this](wxCommandEvent&) { select_tab(ETabType::PlaterGcode); }, "preview_menu", nullptr,
                []() {return true; }, this);
            windowMenu->AppendSeparator();
        }
        append_menu_item(windowMenu, wxID_HIGHEST + 4, _L("P&rint Settings Tab") + "\t" + GUI::shortkey_ctrl_prefix() + "4", _L("Show the print settings"),
            [this/*, tab_offset*/](wxCommandEvent&) { select_tab(ETabType::PrintSettings); }, "cog", nullptr,
            []() {return true; }, this);
        wxMenuItem* item_material_tab = append_menu_item(windowMenu, wxID_HIGHEST + 5, _L("&Filament Settings Tab") + "\t" + GUI::shortkey_ctrl_prefix() + "5", _L("Show the filament settings"),
            [this/*, tab_offset*/](wxCommandEvent&) { select_tab(ETabType::FilamentSettings); }, "spool", nullptr,
            []() {return true; }, this);
        m_changeable_menu_items.push_back(item_material_tab);
        wxMenuItem* item_printer_tab = append_menu_item(windowMenu, wxID_HIGHEST + 6, _L("Print&er Settings Tab") + "\t" + GUI::shortkey_ctrl_prefix() + "6", _L("Show the printer settings"),
            [this/*, tab_offset*/](wxCommandEvent&) { select_tab(ETabType::PrinterSettings); }, "printer", nullptr,
            []() {return true; }, this);
        m_changeable_menu_items.push_back(item_printer_tab);

        windowMenu->AppendSeparator();
        append_menu_item(windowMenu, wxID_ANY, _L("Print &Host Upload Queue") + "\t" + GUI::shortkey_ctrl_prefix() + "J", _L("Display the Print Host Upload Queue window"),
            [this](wxCommandEvent&) { m_printhost_queue_dlg->Show(); }, "upload_queue", nullptr, []() {return true; }, this);
        
        windowMenu->AppendSeparator();
        append_menu_item(windowMenu, wxID_ANY, _L("Open new instance") + "\t" + GUI::shortkey_ctrl_prefix() + "Shift+" + "I", _L("Open a new " SLIC3R_APP_NAME " instance"),
			[this](wxCommandEvent&) { start_new_slicer(); }, "", nullptr, [this]() {return m_plater != nullptr && wxGetApp().app_config->get("single_instance") != "1"; }, this);
    }

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater) {
        viewMenu = new wxMenu();
        add_common_view_menu_items(viewMenu, this, std::bind(&MainFrame::can_change_view, this));
        viewMenu->AppendSeparator();
        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &labels") + "\t" + GUI::shortkey_ctrl_prefix() + "E", _L("Show object/instance labels in 3D scene"),
            [this](wxCommandEvent&) { m_plater->show_view3D_labels(!m_plater->are_view3D_labels_shown()); }, this,
            [this]() { return m_plater->is_view3D_shown(); }, [this]() { return m_plater->are_view3D_labels_shown(); }, this);
        append_menu_check_item(viewMenu, wxID_ANY, _L("&Collapse sidebar") + "\t" + "Shift+" + sep_space + "Tab", _L("Collapse sidebar"),
            [this](wxCommandEvent&) { 
                //fix for trigger on pressed & release
                static time_t last_time_called = 0;
                time_t now = Slic3r::Utils::get_current_time_utc();
                if(now - last_time_called > 2)
                    m_plater->collapse_sidebar(!m_plater->is_sidebar_collapsed());
                last_time_called = now;
            }, this,
            []() { return true; }, [this]() { return m_plater->is_sidebar_collapsed(); }, this);
    }

    // calibration menu
    wxMenu* calibrationMenu = nullptr;
    if (wxGetApp().is_editor())
    {
        calibrationMenu = new wxMenu();
        append_menu_item(calibrationMenu, wxID_ANY, _(L("Introduction")), _(L("How to use this menu and calibrations.")),
            [this](wxCommandEvent&) { wxGetApp().html_dialog(); });
        calibrationMenu->AppendSeparator();
        append_menu_item(calibrationMenu, wxID_ANY, _(L("Bed/Extruder levelling")), _(L("Create a test print to help you to level your printer bed.")),
            [this](wxCommandEvent&) { wxGetApp().bed_leveling_dialog(); });
        calibrationMenu->AppendSeparator();
        append_menu_item(calibrationMenu, wxID_ANY, _(L("Filament Flow calibration")), _(L("Create a test print to help you to set your filament extrusion multiplier.")),
            [this](wxCommandEvent&) { wxGetApp().flow_ratio_dialog(); });
        append_menu_item(calibrationMenu, wxID_ANY, _(L("Filament temperature calibration")), _(L("Create a test print to help you to set your filament temperature.")),
            [this](wxCommandEvent&) { wxGetApp().filament_temperature_dialog(); });
        append_menu_item(calibrationMenu, wxID_ANY, _(L("Extruder retraction calibration")), _(L("Create a test print to help you to set your retraction length.")),
            [this](wxCommandEvent&) { wxGetApp().calibration_retraction_dialog(); });
        calibrationMenu->AppendSeparator();
        append_menu_item(calibrationMenu, wxID_ANY, _(L("Bridge flow calibration")), _(L("Create a test print to help you to set your bridge flow ratio.")),
            [this](wxCommandEvent&) { wxGetApp().bridge_tuning_dialog(); });
        append_menu_item(calibrationMenu, wxID_ANY, _(L("Ironing pattern calibration")), _(L("Create a test print to help you to set your over-bridge flow ratio and ironing pattern.")),
            [this](wxCommandEvent&) { wxGetApp().over_bridge_dialog(); });
        calibrationMenu->AppendSeparator();
        append_menu_item(calibrationMenu, wxID_ANY, _(L("Calibration cube")), _(L("Print a calibration cube, for various calibration goals.")),
            [this](wxCommandEvent&) { wxGetApp().calibration_cube_dialog(); });
    }

    // objects menu
    wxMenu* generationMenu = nullptr;
    if (wxGetApp().is_editor())
    {
        generationMenu  = new wxMenu();
        append_menu_item(generationMenu, wxID_ANY, _(L("FreeCad python script")), _(L("Create an object by writing little easy script.")),
            [this](wxCommandEvent&) { wxGetApp().freecad_script_dialog(); });
        append_menu_item(generationMenu, wxID_ANY, _(L("Script help page")), _(L("How to use the FreeCad python script window.")),
            [this](wxCommandEvent&) { wxLaunchDefaultBrowser("https://github.com/supermerill/SuperSlicer/wiki/FreePySCAD-script-window"); });

    }

    // Help menu
    auto helpMenu = generate_help_menu();

    // menubar
    // assign menubar to frame after appending items, otherwise special items
    // will not be handled correctly
    m_menubar = new wxMenuBar();
    m_menubar->Append(fileMenu, _L("&File"));
    if (editMenu) m_menubar->Append(editMenu, _L("&Edit"));
    m_menubar->Append(windowMenu, _L("&Window"));
    if (viewMenu) m_menubar->Append(viewMenu, _L("&View"));
    if (calibrationMenu) m_menubar->Append(calibrationMenu, _L("C&alibration"));
    if (generationMenu) m_menubar->Append(generationMenu, _L("&Generate"));
    // Add additional menus from C++
    wxGetApp().add_config_menu(m_menubar);
    m_menubar->Append(helpMenu, _L("&Help"));
    SetMenuBar(m_menubar);

#ifdef __APPLE__
    // This fixes a bug on Mac OS where the quit command doesn't emit window close events
    // wx bug: https://trac.wxwidgets.org/ticket/18328
    wxMenu* apple_menu = m_menubar->OSXGetAppleMenu();
    if (apple_menu != nullptr) {
        apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent &) {
            Close();
        }, wxID_EXIT);
    }
#endif // __APPLE__

    if (plater()->printer_technology() == ptSLA)
        update_menubar();
}

void MainFrame::init_menubar_as_gcodeviewer()
{
    wxMenu* fileMenu = new wxMenu;
    {
        append_menu_item(fileMenu, wxID_ANY, _L("&Open G-code") + dots + sep + GUI::shortkey_ctrl_prefix() + "O", _L("Open a G-code file"),
            [this](wxCommandEvent&) { if (m_plater != nullptr) m_plater->load_gcode(); }, "open", nullptr,
            [this]() {return m_plater != nullptr; }, this);
#ifdef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Re&load from disk") + dots + "\tCtrl+Shift+R",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_gcode_from_disk(); },
            "", nullptr, [this]() { return !m_plater->get_last_loaded_gcode().empty(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Re&load from disk") + sep + "F5",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_gcode_from_disk(); },
            "", nullptr, [this]() { return !m_plater->get_last_loaded_gcode().empty(); }, this);
#endif // __APPLE__
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _L("Export &toolpaths as OBJ") + dots, _L("Export toolpaths as OBJ"),
            [this](wxCommandEvent&) { if (m_plater != nullptr) m_plater->export_toolpaths_to_obj(); }, "export_plater", nullptr,
            [this]() {return can_export_toolpaths(); }, this);
        append_menu_item(fileMenu, wxID_ANY, _L("O&pen " SLIC3R_APP_NAME) + dots, _L("Open " SLIC3R_APP_NAME),
            [this](wxCommandEvent&) { start_new_slicer(); }, "", nullptr,
            [this]() {return true; }, this);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_EXIT, _L("&Quit"), wxString::Format(_L("Quit %s"), SLIC3R_APP_NAME),
            [this](wxCommandEvent&) { Close(false); });
    }

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater != nullptr) {
        viewMenu = new wxMenu();
        add_common_view_menu_items(viewMenu, this, std::bind(&MainFrame::can_change_view, this));
    }

    // helpmenu
    auto helpMenu = generate_help_menu();

    m_menubar = new wxMenuBar();
    m_menubar->Append(fileMenu, _L("&File"));
    if (viewMenu != nullptr) m_menubar->Append(viewMenu, _L("&View"));
    // Add additional menus from C++
    wxGetApp().add_config_menu(m_menubar);
    m_menubar->Append(helpMenu, _L("&Help"));
    SetMenuBar(m_menubar);

#ifdef __APPLE__
    // This fixes a bug on Mac OS where the quit command doesn't emit window close events
    // wx bug: https://trac.wxwidgets.org/ticket/18328
    wxMenu* apple_menu = m_menubar->OSXGetAppleMenu();
    if (apple_menu != nullptr) {
        apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            Close();
            }, wxID_EXIT);
    }
#endif // __APPLE__
}

void MainFrame::update_menubar()
{
    if (wxGetApp().is_gcode_viewer())
        return;

    const bool is_fff = plater()->printer_technology() == ptFFF;

    m_changeable_menu_items[miExport]       ->SetItemLabel((is_fff ? _L("Export &G-code")         : _L("E&xport"))        + dots    + "\t" + GUI::shortkey_ctrl_prefix() + "G");
    m_changeable_menu_items[miSend]         ->SetItemLabel((is_fff ? _L("S&end G-code")           : _L("S&end to print")) + dots    + "\t" + GUI::shortkey_ctrl_prefix() + "Shift+G");

    m_changeable_menu_items[miMaterialTab]  ->SetItemLabel((is_fff ? _L("&Filament Settings Tab") : _L("Mate&rial Settings Tab"))   + "\t" + GUI::shortkey_ctrl_prefix() + "5");
    m_changeable_menu_items[miMaterialTab]  ->SetBitmap(create_scaled_bitmap(is_fff ? "spool"   : "resin"));

    m_changeable_menu_items[miPrinterTab]   ->SetBitmap(create_scaled_bitmap(is_fff ? "printer" : "sla_printer"));
}

// To perform the "Quck Slice", "Quick Slice and Save As", "Repeat last Quick Slice" and "Slice to SVG".
void MainFrame::quick_slice(const int qs)
{
//     my $progress_dialog;
    wxString input_file;
//  eval
//     {
    // validate configuration
    auto config = wxGetApp().preset_bundle->full_config();
    auto valid = config.validate();
    if (! valid.empty()) {
        show_error(this, valid);
        return;
    }

    // select input file
    if (!(qs & qsReslice)) {
        wxFileDialog dlg(this, _L("Choose a file to slice (STL/OBJ/AMF/3MF/PRUSA):"),
            wxGetApp().app_config->get_last_dir(), "",
            file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        input_file = dlg.GetPath();
        if (!(qs & qsExportSVG))
            m_qs_last_input_file = input_file;
    }
    else {
        if (m_qs_last_input_file.IsEmpty()) {
            wxMessageDialog dlg(this, _L("No previously sliced file."),
                _L("Error"), wxICON_ERROR | wxOK);
            dlg.ShowModal();
            return;
        }
        if (std::ifstream(m_qs_last_input_file.ToUTF8().data())) {
            wxMessageDialog dlg(this, _L("Previously sliced file (")+m_qs_last_input_file+_L(") not found."),
                _L("File Not Found"), wxICON_ERROR | wxOK);
            dlg.ShowModal();
            return;
        }
        input_file = m_qs_last_input_file;
    }
    auto input_file_basename = get_base_name(input_file);
    wxGetApp().app_config->update_skein_dir(get_dir_name(input_file));

    auto bed_shape = Slic3r::Polygon::new_scale(config.option<ConfigOptionPoints>("bed_shape")->values);
//     auto print_center = Slic3r::Pointf->new_unscale(bed_shape.bounding_box().center());
// 
//     auto sprint = new Slic3r::Print::Simple(
//         print_center = > print_center,
//         status_cb = > [](int percent, const wxString& msg) {
//         m_progress_dialog->Update(percent, msg+"…");
//     });

    // keep model around
    auto model = Slic3r::Model::read_from_file(input_file.ToUTF8().data());

//     sprint->apply_config(config);
//     sprint->set_model(model);

    // Copy the names of active presets into the placeholder parser.
//     wxGetApp().preset_bundle->export_selections(sprint->placeholder_parser);

    // select output file
    wxString output_file;
    if (qs & qsReslice) {
        if (!m_qs_last_output_file.IsEmpty())
            output_file = m_qs_last_output_file;
    } 
    else if (qs & qsSaveAs) {
        // The following line may die if the output_filename_format template substitution fails.
        wxFileDialog dlg(this, from_u8((boost::format(_utf8(L("Save %s file as:"))) % ((qs & qsExportSVG) ? _L("SVG") : _L("G-code"))).str()),
            wxGetApp().app_config->get_last_output_dir(get_dir_name(output_file)), get_base_name(input_file), 
            qs & qsExportSVG ? file_wildcards(FT_SVG) : file_wildcards(FT_GCODE),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        output_file = dlg.GetPath();
        if (!(qs & qsExportSVG))
            m_qs_last_output_file = output_file;
        wxGetApp().app_config->update_last_output_dir(get_dir_name(output_file));
    } 
    else if (qs & qsExportPNG) {
        wxFileDialog dlg(this, _L("Save zip file as:"),
            wxGetApp().app_config->get_last_output_dir(get_dir_name(output_file)),
            get_base_name(output_file), "*.sl1", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        output_file = dlg.GetPath();
        }

    // show processbar dialog
    m_progress_dialog = new wxProgressDialog(_L("Slicing") + dots,
    // TRN "Processing input_file_basename"
                                             from_u8((boost::format(_utf8(L("Processing %s"))) % (input_file_basename + dots)).str()),
        100, nullptr, wxPD_AUTO_HIDE);
    m_progress_dialog->Pulse();
    {
//         my @warnings = ();
//         local $SIG{ __WARN__ } = sub{ push @warnings, $_[0] };

//         sprint->output_file(output_file);
//         if (export_svg) {
//             sprint->export_svg();
//         }
//         else if(export_png) {
//             sprint->export_png();
//         }
//         else {
//             sprint->export_gcode();
//         }
//         sprint->status_cb(undef);
//         Slic3r::GUI::warning_catcher($self)->($_) for @warnings;
    }
    m_progress_dialog->Destroy();
    m_progress_dialog = nullptr;

    auto message = format(_L("%1% was successfully sliced."), input_file_basename);
//     wxTheApp->notify(message);
    wxMessageDialog(this, message, _L("Slicing Done!"), wxOK | wxICON_INFORMATION).ShowModal();
//     };
//     Slic3r::GUI::catch_error(this, []() { if (m_progress_dialog) m_progress_dialog->Destroy(); });
}

void MainFrame::reslice_now()
{
    if (m_plater)
        m_plater->reslice();
}

void MainFrame::repair_stl()
{
    wxString input_file;
    {
        wxFileDialog dlg(this, _L("Select the STL file to repair:"),
            wxGetApp().app_config->get_last_dir(), "",
            file_wildcards(FT_STL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        input_file = dlg.GetPath();
        }

    wxString output_file = input_file;
    {
        wxFileDialog dlg( this, L("Save OBJ file (less prone to coordinate errors than STL) as:"),
                                        get_dir_name(output_file), get_base_name(output_file, ".obj"),
                                        file_wildcards(FT_OBJ), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        output_file = dlg.GetPath();
        }

    auto tmesh = new Slic3r::TriangleMesh();
    tmesh->ReadSTLFile(input_file.ToUTF8().data());
    tmesh->repair();
    tmesh->WriteOBJFile(output_file.ToUTF8().data());
    Slic3r::GUI::show_info(this, L("Your file was repaired."), L("Repair"));
}

void MainFrame::export_config(bool to_prusa)
{
    // Generate a cummulative configuration for the selected print, filaments and printer.
    auto config = wxGetApp().preset_bundle->full_config();
    // Validate the cummulative configuration.
    auto valid = config.validate();
    if (! valid.empty()) {
        show_error(this, valid);
        return;
    }
    // Ask user for the file name for the config file.
    wxFileDialog dlg(this, _L("Save configuration as:"),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
        !m_last_config.IsEmpty() ? get_base_name(m_last_config) : "config.ini",
        file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    wxString file;
    if (dlg.ShowModal() == wxID_OK)
        file = dlg.GetPath();
    if (!file.IsEmpty()) {
        wxGetApp().app_config->update_config_dir(get_dir_name(file));
        m_last_config = file;
        config.save(file.ToUTF8().data(), to_prusa);
    }
}

// Load a config file containing a Print, Filament & Printer preset.
void MainFrame::load_config_file()
{
        if (!wxGetApp().check_unsaved_changes())
            return;
    wxFileDialog dlg(this, _L("Select configuration to load:"),
            !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
            "config.ini", "INI files (*.ini, *.gcode)|*.ini;*.INI;*.gcode;*.g", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	wxString file;
    if (dlg.ShowModal() == wxID_OK)
        file = dlg.GetPath();
    if (! file.IsEmpty() && this->load_config_file(file.ToUTF8().data())) {
        wxGetApp().app_config->update_config_dir(get_dir_name(file));
        m_last_config = file;
    }
}

// Load a config file containing a Print, Filament & Printer preset from command line.
bool MainFrame::load_config_file(const std::string &path)
{
    try {
        wxGetApp().preset_bundle->load_config_file(path); 
    } catch (const std::exception& ex) {
        show_error(this, ex.what());
        return false;
    }
	wxGetApp().load_current_presets();
    return true;
}

void MainFrame::export_configbundle(bool export_physical_printers /*= false*/)
{
    if (!wxGetApp().check_unsaved_changes())
        return;
    // validate current configuration in case it's dirty
    auto err = wxGetApp().preset_bundle->full_config().validate();
    if (! err.empty()) {
        show_error(this, err);
        return;
    }
    // Ask user for a file name.
    wxFileDialog dlg(this, _L("Save presets bundle as:"),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
        SLIC3R_APP_KEY "_config_bundle.ini",
        file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    wxString file;
    if (dlg.ShowModal() == wxID_OK)
        file = dlg.GetPath();
    if (!file.IsEmpty()) {
        // Export the config bundle.
        wxGetApp().app_config->update_config_dir(get_dir_name(file));
        try {
            wxGetApp().preset_bundle->export_configbundle(file.ToUTF8().data(), false, export_physical_printers);
        } catch (const std::exception& ex) {
			show_error(this, ex.what());
        }
    }
}

// Loading a config bundle with an external file name used to be used
// to auto - install a config bundle on a fresh user account,
// but that behavior was not documented and likely buggy.
void MainFrame::load_configbundle(wxString file/* = wxEmptyString, const bool reset_user_profile*/)
{
    if (!wxGetApp().check_unsaved_changes())
        return;
    if (file.IsEmpty()) {
        wxFileDialog dlg(this, _L("Select configuration to load:"),
            !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
            "config.ini", file_wildcards(FT_INI), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        file = dlg.GetPath();
		}

    wxGetApp().app_config->update_config_dir(get_dir_name(file));

    auto presets_imported = 0;
    try {
        presets_imported = wxGetApp().preset_bundle->load_configbundle(file.ToUTF8().data());
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return;
    }

    // Load the currently selected preset into the GUI, update the preset selection box.
	wxGetApp().load_current_presets();

    const auto message = wxString::Format(_L("%d presets successfully imported."), presets_imported);
    Slic3r::GUI::show_info(this, message, wxString("Info"));
}

// Load a provied DynamicConfig into the Print / Filament / Printer tabs, thus modifying the active preset.
// Also update the plater with the new presets.
void MainFrame::load_config(const DynamicPrintConfig& config)
{
	PrinterTechnology printer_technology = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();
	const auto       *opt_printer_technology = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
	if (opt_printer_technology != nullptr && opt_printer_technology->value != printer_technology) {
		printer_technology = opt_printer_technology->value;
		this->plater()->set_printer_technology(printer_technology);
	}
#if 0
    for (auto tab : wxGetApp().tabs_list)
		if (tab->supports_printer_technology(printer_technology)) {
			if (tab->type() == Slic3r::Preset::TYPE_PRINTER)
				static_cast<TabPrinter*>(tab)->update_pages();
        tab->load_config(config);
		}
    if (m_plater)
        m_plater->on_config_change(config);
#else
	// Load the currently selected preset into the GUI, update the preset selection box.
    //FIXME this is not quite safe for multi-extruder printers,
    // as the number of extruders is not adjusted for the vector values.
    // (see PresetBundle::update_multi_material_filament_presets())
    // Better to call PresetBundle::load_config() instead?
    for (auto tab : wxGetApp().tabs_list)
        if (tab->supports_printer_technology(printer_technology)) {
            // Only apply keys, which are present in the tab's config. Ignore the other keys.
			for (const std::string &opt_key : tab->get_config()->diff(config))
				// Ignore print_settings_id, printer_settings_id, filament_settings_id etc.
				if (! boost::algorithm::ends_with(opt_key, "_settings_id"))
					tab->get_config()->option(opt_key)->set(config.option(opt_key));
        }
    
    wxGetApp().load_current_presets();
#endif
}

void MainFrame::select_tab(Tab* tab)
{
    if (!tab)
        return;
    ETabType tab_type = ETabType::LastSettings;
    switch (tab->type()) {
    case Preset::Type::TYPE_FILAMENT:
    case Preset::Type::TYPE_SLA_MATERIAL:
        tab_type = ETabType::FilamentSettings;
    case Preset::Type::TYPE_PRINT:
    case Preset::Type::TYPE_SLA_PRINT:
        tab_type = ETabType::PrintSettings;
    case Preset::Type::TYPE_PRINTER:
        tab_type = ETabType::PrinterSettings;
    }
    select_tab(tab_type);

}

MainFrame::ETabType MainFrame::selected_tab() const
{
    if (m_layout == ESettingsLayout::Old) {
        if (m_tabpanel->GetSelection() == 0)
            if (m_plater->is_view3D_shown())
                return ETabType::Plater3D;
            else
                return ETabType::PlaterGcode;
        else
            return ETabType((uint8_t)ETabType::PrintSettings + m_tabpanel->GetSelection() - 1);
    }
    else if (m_layout == ESettingsLayout::Tabs) {
        if (m_tabpanel->GetSelection() < 3)
            return ETabType((uint8_t)ETabType::Plater3D + m_tabpanel->GetSelection());
        else
            return ETabType((uint8_t)ETabType::PrintSettings + m_tabpanel->GetSelection() - 3);
    }
    else if (m_layout == ESettingsLayout::Hidden) {
        if (!m_main_sizer->IsShown(m_tabpanel)) {
            if (m_plater->is_view3D_shown())
                return ETabType::Plater3D;
            else
                return ETabType::PlaterGcode;
        } else {
            return ETabType((uint8_t)ETabType::PrintSettings + m_tabpanel->GetSelection() - 1);
        }
    }
    else if (m_layout == ESettingsLayout::Dlg) {
        if (!m_main_sizer->IsShown(m_tabpanel)) {
            if (m_plater->is_view3D_shown())
                return ETabType::Plater3D;
            else
                return ETabType::PlaterGcode;
        }
    }
}

void MainFrame::select_tab(ETabType tab /* = Any*/, bool keep_tab_type)
{
    bool tabpanel_was_hidden = false;

    // Controls on page are created on active page of active tab now.
    // We should select/activate tab before its showing to avoid an UI-flickering
    auto select = [this, tab](bool was_hidden) {
        // when tab == -1, it means we should show the last selected tab
        size_t new_selection = 0;
        if (tab <= ETabType::LastPlater) {
            //select plater
            new_selection = (uint8_t)tab;
            if (tab == ETabType::LastPlater)
                new_selection = m_last_selected_plater_tab > 2 ? 0 : m_last_selected_plater_tab;
            if (m_layout != ESettingsLayout::Tabs)
                new_selection = 0;

        } else if (tab <= ETabType::LastSettings) {
            //select setting
            new_selection = (uint8_t)tab - (uint8_t)ETabType::PrintSettings;
            if (tab == ETabType::LastSettings) 
                new_selection = m_last_selected_setting_tab > 2 ? 0 : m_last_selected_setting_tab;
            //push to the correct position
            if (m_layout == ESettingsLayout::Tabs)
                new_selection = new_selection + 3;
            else if (m_layout != ESettingsLayout::Dlg)
                new_selection = new_selection + 1;
        }

        if (m_tabpanel->GetSelection() != (int)new_selection)
            m_tabpanel->SetSelection(new_selection);
        else if (was_hidden) {
            Tab* cur_tab = dynamic_cast<Tab*>(m_tabpanel->GetPage(new_selection));
            if (cur_tab)
                cur_tab->OnActivate();
        }
    };

    if (m_layout != ESettingsLayout::Tabs) {
        if (tab == ETabType::Plater3D || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 0)) {
            m_plater->select_view_3D("3D");
        } else if (tab == ETabType::PlaterPreview || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 1)) {
            m_plater->select_view_3D("Preview");
        } else if (tab == ETabType::PlaterGcode || (tab == ETabType::LastPlater && m_last_selected_plater_tab == 2)) {
            m_plater->select_view_3D("Preview");
        }
    }

    if (m_layout == ESettingsLayout::Dlg) {
        if (keep_tab_type)
            return;
        if (tab <= ETabType::LastPlater) {
            if (m_settings_dialog.IsShown())
                this->SetFocus();
            // plater should be focused for correct navigation inside search window
            if (m_plater->canvas3D()->is_search_pressed())
                m_plater->SetFocus();
            return;
        }
        // Show/Activate Settings Dialog
#ifdef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
        if (m_settings_dialog.IsShown())
            m_settings_dialog.Hide();
        else
            tabpanel_was_hidden = true;
            
        select(tabpanel_was_hidden);
        m_tabpanel->Show();
        m_settings_dialog.Show();
#else
        if (m_settings_dialog.IsShown()) {
            select(false);
            m_settings_dialog.SetFocus();
        }
        else {
            tabpanel_was_hidden = true;
            select(tabpanel_was_hidden);
            m_tabpanel->Show();
            m_settings_dialog.Show();
        }
#endif
    }
    else if (m_layout == ESettingsLayout::Hidden) {
        if (keep_tab_type && m_tabpanel->GetSelection()>0)
            return;
        m_main_sizer->Show(m_plater, tab <= ETabType::LastPlater);
        tabpanel_was_hidden = !m_main_sizer->IsShown(m_tabpanel);
        select(tabpanel_was_hidden);
        m_main_sizer->Show(m_tabpanel, tab > ETabType::LastPlater);

        // plater should be focused for correct navigation inside search window
        if (tab <= ETabType::LastPlater && m_plater->canvas3D()->is_search_pressed())
            m_plater->SetFocus();
        Layout();
    }
    else if (m_layout == ESettingsLayout::Old) {
        if (keep_tab_type && m_tabpanel->GetSelection() > 0)
            return;
        else
            select(false);
    }
    else if (m_layout == ESettingsLayout::Tabs) {
        if (keep_tab_type && ( (m_tabpanel->GetSelection() >=3 && tab <= ETabType::LastPlater) || (m_tabpanel->GetSelection() < 3 && tab > ETabType::LastPlater)))
            return;
        else
            select(false);
    }
    else
        select(false);

    // When we run application in ESettingsLayout::Hidden or ESettingsLayout::Dlg mode, tabpanel is hidden from the very beginning
    // and as a result Tab::update_changed_tree_ui() function couldn't update m_is_nonsys_values values,
    // which are used for update TreeCtrl and "revert_buttons".
    // So, force the call of this function for Tabs, if tab panel was hidden
    if (tabpanel_was_hidden)
        for (auto cur_tab : wxGetApp().tabs_list)
            cur_tab->update_changed_tree_ui();

    //// when tab == -1, it means we should show the last selected tab
    //size_t new_selection = tab == (size_t)(-1) ? m_last_selected_tab : (m_layout == ESettingsLayout::Dlg && tab != 0) ? tab - 1 : tab;
    //if (m_tabpanel->GetSelection() != new_selection)
    //    m_tabpanel->SetSelection(new_selection);
    //if (tabpanel_was_hidden)
    //    static_cast<Tab*>(m_tabpanel->GetPage(new_selection))->OnActivate();
}

// Set a camera direction, zoom to all objects.
void MainFrame::select_view(const std::string& direction)
{
     if (m_plater)
         m_plater->select_view(direction);
}

// #ys_FIXME_to_delete
void MainFrame::on_presets_changed(SimpleEvent &event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr) {
        return;
    }

    // Update preset combo boxes(Print settings, Filament, Material, Printer) from their respective tabs.
    auto presets = tab->get_presets();
    if (m_plater != nullptr && presets != nullptr) {

        // FIXME: The preset type really should be a property of Tab instead
        Slic3r::Preset::Type preset_type = tab->type();
        if (preset_type == Slic3r::Preset::TYPE_INVALID) {
            wxASSERT(false);
            return;
        }

        m_plater->on_config_change(*tab->get_config());
        m_plater->sidebar().update_presets(preset_type);
    }
}

// #ys_FIXME_to_delete
void MainFrame::on_value_changed(wxCommandEvent& event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr)
        return;

    auto opt_key = event.GetString();
    if (m_plater) {
        m_plater->on_config_change(*tab->get_config()); // propagate config change events to the plater
        if (opt_key == "extruders_count") {
            auto value = event.GetInt();
            //to update filaments gui
            m_plater->on_extruders_change(value);
        }
    }
}

void MainFrame::on_config_changed(DynamicPrintConfig* config) const
{
    if (m_plater)
        m_plater->on_config_change(*config); // propagate config change events to the plater
}

void MainFrame::add_to_recent_projects(const wxString& filename)
{
    if (wxFileExists(filename))
    {
        m_recent_projects.AddFileToHistory(filename);
        std::vector<std::string> recent_projects;
        size_t count = m_recent_projects.GetCount();
        for (size_t i = 0; i < count; ++i)
        {
            recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
        }
        wxGetApp().app_config->set_recent_projects(recent_projects);
        wxGetApp().app_config->save();
    }
}

//
// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void MainFrame::update_ui_from_settings(bool apply_free_camera_correction)
{
//    const bool bp_on = wxGetApp().app_config->get("background_processing") == "1";
//     m_menu_item_reslice_now->Enable(!bp_on);
//    m_plater->sidebar().show_reslice(!bp_on);
//    m_plater->sidebar().show_export(bp_on);
//    m_plater->sidebar().Layout();

    if (m_plater)
        m_plater->update_ui_from_settings(apply_free_camera_correction);
    for (auto tab: wxGetApp().tabs_list)
        tab->update_ui_from_settings();
}

std::string MainFrame::get_base_name(const wxString &full_name, const char *extension) const 
{
    boost::filesystem::path filename = boost::filesystem::path(full_name.wx_str()).filename();
    if (extension != nullptr)
		filename = filename.replace_extension(extension);
    return filename.string();
}

std::string MainFrame::get_dir_name(const wxString &full_name) const 
{
    return boost::filesystem::path(full_name.wx_str()).parent_path().string();
}


// ----------------------------------------------------------------------------
// SettingsDialog
// ----------------------------------------------------------------------------

SettingsDialog::SettingsDialog(MainFrame* mainframe)
: DPIDialog(mainframe, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _L("Settings"), wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX, "settings_dialog"),
    m_main_frame(mainframe)
{
    if (wxGetApp().is_gcode_viewer())
        return;

#if ENABLE_WX_3_1_3_DPI_CHANGED_EVENT && defined(__WXMSW__)
    // ys_FIXME! temporary workaround for correct font scaling
    // Because of from wxWidgets 3.1.3 auto rescaling is implemented for the Fonts,
    // From the very beginning set dialog font to the wxSYS_DEFAULT_GUI_FONT
    this->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT));
#else
    this->SetFont(wxGetApp().normal_font());
#endif // ENABLE_WX_3_1_3_DPI_CHANGED_EVENT
    this->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

    // Load the icon either from the exe, or from the ico file.
#if _WIN32
    {
        TCHAR szExeFileName[MAX_PATH];
        GetModuleFileName(nullptr, szExeFileName, MAX_PATH);
        SetIcon(wxIcon(szExeFileName, wxBITMAP_TYPE_ICO));
    }
#else
    SetIcon(wxIcon(var(SLIC3R_APP_KEY "_128px.png"), wxBITMAP_TYPE_PNG));
#endif // _WIN32

    this->Bind(wxEVT_SHOW, [this](wxShowEvent& evt) {

        auto key_up_handker = [this](wxKeyEvent& evt) {
            if ((evt.GetModifiers() & wxMOD_CONTROL) != 0) {
                switch (evt.GetKeyCode()) {
                case '1': { m_main_frame->select_tab(MainFrame::ETabType::Plater3D); break; }
                case '2': { m_main_frame->select_tab(MainFrame::ETabType::PlaterPreview); break; }
                case '3': { m_main_frame->select_tab(MainFrame::ETabType::PlaterGcode); break; }
                case '4': { m_main_frame->select_tab(MainFrame::ETabType::PrintSettings); break; }
                case '5': { m_main_frame->select_tab(MainFrame::ETabType::FilamentSettings); break; }
                case '6': { m_main_frame->select_tab(MainFrame::ETabType::PrinterSettings); break; }
#ifdef __APPLE__
                case 'f':
#else /* __APPLE__ */
                case WXK_CONTROL_F:
#endif /* __APPLE__ */
                case 'F': { m_main_frame->plater()->search(false); break; }
                default:break;
                }
            }
        };

        if (evt.IsShown()) {
            if (m_tabpanel != nullptr)
                m_tabpanel->Bind(wxEVT_KEY_UP, key_up_handker);
        }
        else {
            if (m_tabpanel != nullptr)
                m_tabpanel->Unbind(wxEVT_KEY_UP, key_up_handker);
        }
        });

    // initialize layout
    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetSizeHints(this);
    SetSizer(sizer);
    Fit();

    const wxSize min_size = wxSize(85 * em_unit(), 50 * em_unit());
#ifdef __APPLE__
    // Using SetMinSize() on Mac messes up the window position in some cases
    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
    SetSize(min_size);
#else
    SetMinSize(min_size);
    SetSize(GetMinSize());
#endif
    Layout();
}

void SettingsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    if (wxGetApp().is_gcode_viewer())
        return;

    const int& em = em_unit();
    const wxSize& size = wxSize(85 * em, 50 * em);

    // update Tabs
    for (auto tab : wxGetApp().tabs_list)
        tab->msw_rescale();

    SetMinSize(size);
    Fit();
    Refresh();
}


} // namespace GUI
} // namespace Slic3r
