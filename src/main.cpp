#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Font.h>
#include <GroupLayoutBuilder.h>
#include <GroupView.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <OS.h>
#include <Path.h>
#include <ScrollView.h>
#include <SeparatorItem.h>
#include <Size.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

static const uint32 MSG_CONNECT = 'conn';
static const uint32 MSG_DISCONNECT = 'disc';
static const uint32 MSG_SETTINGS = 'sett';
static const uint32 MSG_SAVE_SETTINGS = 'ssav';
static const uint32 MSG_CANCEL_SETTINGS = 'scan';
static const uint32 MSG_PROMPT_SUBMIT = 'psub';
static const uint32 MSG_PROMPT_CANCEL = 'pcan';
static const uint32 MSG_PROCESS_OUTPUT = 'pout';
static const uint32 MSG_PROCESS_EXIT = 'pext';
static const uint32 MSG_TOGGLE_LOG = 'tlog';
static const uint32 MSG_QUIT = 'quit';
static const uint32 MSG_APP_QUIT_NOW = 'aqit';
static const uint32 MSG_BROWSE_SCRIPT = 'bscr';
static const uint32 MSG_SETTINGS_CLOSED = 'scls';

static const char* kAppSignature = "application/x-vnd.BooConnect";
static const char* kBaseDir = "/boot/home/config/non-packaged/BooConnect";
static const char* kSettingsPath = "/boot/home/config/non-packaged/BooConnect/settings.json";
static const char* kIconPath = "./Assets/AppIcon.hvif";
static const float kWindowWidth = 450;
static const float kWindowHeight = 500;
static const float kIconSize = 128;
static const rgb_color kStatusRed = {210, 44, 36, 255};
static const rgb_color kStatusGreen = {40, 150, 70, 255};
static const rgb_color kStatusNeutral = {40, 40, 40, 255};

struct Settings {
	std::string server;
	std::string user;
	std::string password;
	std::string iface = "tun/0";
	std::string userAgent = "AnyConnect";
	std::string script = "vpnc-script";
	std::string extraArgs;
	bool verbose = true;
	float windowX = 80;
	float windowY = 80;
};

static std::string
Trim(const std::string& value)
{
	size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return "";
	size_t last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

static bool
IsConfigured(const Settings& settings)
{
	return !Trim(settings.server).empty() && !Trim(settings.user).empty();
}

static std::string
Lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

static bool
IsOpenConnectRunning()
{
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		std::string name = info.name;
		std::string args = info.args;
		if (name.find("openconnect") != std::string::npos
			|| args.find("openconnect") != std::string::npos) {
			return true;
		}
	}
	return false;
}


static std::string
JsonEscape(const std::string& value)
{
	std::string out;
	for (char c : value) {
		switch (c) {
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: out += c; break;
		}
	}
	return out;
}

static std::string
JsonString(const std::string& text, const char* key, const std::string& fallback)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t keyPos = text.find(needle);
	if (keyPos == std::string::npos)
		return fallback;
	size_t colon = text.find(':', keyPos + needle.size());
	if (colon == std::string::npos)
		return fallback;
	size_t quote = text.find('"', colon + 1);
	if (quote == std::string::npos)
		return fallback;

	std::string out;
	bool escape = false;
	for (size_t i = quote + 1; i < text.size(); ++i) {
		char c = text[i];
		if (escape) {
			switch (c) {
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				default: out += c; break;
			}
			escape = false;
		} else if (c == '\\') {
			escape = true;
		} else if (c == '"') {
			return out;
		} else {
			out += c;
		}
	}
	return fallback;
}

static bool
JsonBool(const std::string& text, const char* key, bool fallback)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t keyPos = text.find(needle);
	if (keyPos == std::string::npos)
		return fallback;
	size_t colon = text.find(':', keyPos + needle.size());
	if (colon == std::string::npos)
		return fallback;
	std::string rest = Trim(text.substr(colon + 1, 8));
	if (rest.rfind("true", 0) == 0)
		return true;
	if (rest.rfind("false", 0) == 0)
		return false;
	return fallback;
}

static float
JsonFloat(const std::string& text, const char* key, float fallback)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t keyPos = text.find(needle);
	if (keyPos == std::string::npos)
		return fallback;
	size_t colon = text.find(':', keyPos + needle.size());
	if (colon == std::string::npos)
		return fallback;
	char* end = NULL;
	float value = strtof(text.c_str() + colon + 1, &end);
	if (end == text.c_str() + colon + 1)
		return fallback;
	return value;
}

static Settings
LoadSettings()
{
	Settings settings;
	std::ifstream in(kSettingsPath);
	if (!in)
		return settings;

	std::stringstream buffer;
	buffer << in.rdbuf();
	std::string text = buffer.str();
	settings.server = JsonString(text, "server", settings.server);
	settings.user = JsonString(text, "user", settings.user);
	settings.password = JsonString(text, "password", settings.password);
	settings.iface = JsonString(text, "interface", settings.iface);
	settings.userAgent = JsonString(text, "useragent", settings.userAgent);
	settings.script = JsonString(text, "script", settings.script);
	settings.extraArgs = JsonString(text, "extra_args", settings.extraArgs);
	settings.verbose = JsonBool(text, "verbose", settings.verbose);
	settings.windowX = JsonFloat(text, "window_x", settings.windowX);
	settings.windowY = JsonFloat(text, "window_y", settings.windowY);
	return settings;
}

static void
SaveSettings(const Settings& settings)
{
	std::ofstream out(kSettingsPath);
	out << "{\n"
		<< "  \"server\": \"" << JsonEscape(settings.server) << "\",\n"
		<< "  \"user\": \"" << JsonEscape(settings.user) << "\",\n"
		<< "  \"password\": \"" << JsonEscape(settings.password) << "\",\n"
		<< "  \"interface\": \"" << JsonEscape(settings.iface) << "\",\n"
		<< "  \"useragent\": \"" << JsonEscape(settings.userAgent) << "\",\n"
		<< "  \"script\": \"" << JsonEscape(settings.script) << "\",\n"
		<< "  \"extra_args\": \"" << JsonEscape(settings.extraArgs) << "\",\n"
		<< "  \"verbose\": " << (settings.verbose ? "true" : "false") << ",\n"
		<< "  \"window_x\": " << settings.windowX << ",\n"
		<< "  \"window_y\": " << settings.windowY << "\n"
		<< "}\n";
}

static std::vector<std::string>
SplitArgs(const std::string& value)
{
	std::vector<std::string> args;
	std::string current;
	bool inQuote = false;
	char quote = 0;
	for (char c : value) {
		if (inQuote) {
			if (c == quote) {
				inQuote = false;
			} else {
				current += c;
			}
		} else if (c == '\'' || c == '"') {
			inQuote = true;
			quote = c;
		} else if (std::isspace((unsigned char)c)) {
			if (!current.empty()) {
				args.push_back(current);
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty())
		args.push_back(current);
	return args;
}

static bool
HasArg(int argc, char** argv, const char* name)
{
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], name) == 0)
			return true;
	}
	return false;
}

static void
DetachFromTerminalIfNeeded(int argc, char** argv)
{
	if (HasArg(argc, argv, "--foreground"))
		return;

	pid_t pid = fork();
	if (pid < 0)
		return;
	if (pid > 0)
		_exit(0);

	setsid();
	int nullFd = open("/dev/null", O_RDWR);
	if (nullFd >= 0) {
		dup2(nullFd, STDIN_FILENO);
		dup2(nullFd, STDOUT_FILENO);
		dup2(nullFd, STDERR_FILENO);
		if (nullFd > STDERR_FILENO)
			close(nullFd);
	}
}

static const char*
FindIconPath()
{
	return kIconPath;
}

class IconView : public BView {
public:
	IconView(const char* path, float size)
		:
		BView("icon", B_WILL_DRAW),
		fBitmap(NULL),
		fSize(size)
	{
		std::ifstream input(path, std::ios::binary);
		if (input) {
			std::vector<uint8> data((std::istreambuf_iterator<char>(input)),
				std::istreambuf_iterator<char>());
			if (!data.empty()) {
				fBitmap = new BBitmap(BRect(0, 0, fSize - 1, fSize - 1),
					B_RGBA32);
				if (BIconUtils::GetVectorIcon(data.data(), data.size(),
						fBitmap) != B_OK) {
					delete fBitmap;
					fBitmap = NULL;
				}
			}
		}

		SetExplicitMinSize(BSize(fSize, fSize));
		SetExplicitPreferredSize(BSize(fSize, fSize));
		SetExplicitMaxSize(BSize(fSize, fSize));
	}

	~IconView() override
	{
		delete fBitmap;
	}

	void Draw(BRect updateRect) override
	{
		BView::Draw(updateRect);
		if (fBitmap == NULL)
			return;

		BRect bounds = Bounds();
		BRect src = fBitmap->Bounds();
		SetDrawingMode(B_OP_ALPHA);
		SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		DrawBitmap(fBitmap, src, bounds);
		SetDrawingMode(B_OP_COPY);
	}

	bool HasIcon() const
	{
		return fBitmap != NULL;
	}

private:
	BBitmap* fBitmap;
	float fSize;
};

class PromptWindow : public BWindow {
public:
	PromptWindow(BWindow* owner, const char* title, const char* label, bool hide)
		:
		BWindow(BRect(0, 0, 360, 130), title, B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_RESIZABLE | B_CLOSE_ON_ESCAPE),
		fOwner(owner)
	{
		fInput = new BTextControl("input", label, "", NULL);
		if (hide)
			fInput->TextView()->HideTyping(true);

		BButton* cancel = new BButton("Cancel", new BMessage(MSG_PROMPT_CANCEL));
		BButton* submit = new BButton("OK", new BMessage(MSG_PROMPT_SUBMIT));
		submit->MakeDefault(true);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 12)
			.SetInsets(12)
			.Add(fInput)
			.AddGroup(B_HORIZONTAL, 8)
				.AddGlue()
				.Add(cancel)
				.Add(submit)
			.End();

		CenterOnScreen();
		fInput->MakeFocus(true);
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case MSG_PROMPT_SUBMIT:
			{
				BMessage submit(MSG_PROMPT_SUBMIT);
				submit.AddString("value", fInput->Text());
				fOwner->PostMessage(&submit);
				Quit();
				break;
			}
			case MSG_PROMPT_CANCEL:
				fOwner->PostMessage(MSG_PROMPT_CANCEL);
				Quit();
				break;
			default:
				BWindow::MessageReceived(message);
				break;
		}
	}

private:
	BWindow* fOwner;
	BTextControl* fInput;
};

class SettingsWindow : public BWindow {
public:
	SettingsWindow(BWindow* owner, const Settings& settings)
		:
		BWindow(BRect(0, 0, 520, 310), "BooConnect Settings", B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_RESIZABLE | B_CLOSE_ON_ESCAPE),
		fOwner(owner),
		fScriptPanel(NULL),
		fClosedNotified(false)
	{
		fServer = new BTextControl("server", "Server", settings.server.c_str(), NULL);
		fUser = new BTextControl("user", "User", settings.user.c_str(), NULL);
		fPassword = new BTextControl("password", "Password/PIN", settings.password.c_str(), NULL);
		fPassword->TextView()->HideTyping(true);
		fInterface = new BTextControl("interface", "Interface", settings.iface.c_str(), NULL);
		fUserAgent = new BTextControl("useragent", "User agent", settings.userAgent.c_str(), NULL);
		fScript = new BTextControl("script", "Script", settings.script.c_str(), NULL);
		BButton* browseScript = new BButton("Browse" B_UTF8_ELLIPSIS,
			new BMessage(MSG_BROWSE_SCRIPT));
		fExtraArgs = new BTextControl("extra", "Extra args", settings.extraArgs.c_str(), NULL);
		fVerbose = new BCheckBox("Verbose openconnect output", NULL);
		fVerbose->SetValue(settings.verbose ? B_CONTROL_ON : B_CONTROL_OFF);

		BButton* cancel = new BButton("Cancel", new BMessage(MSG_CANCEL_SETTINGS));
		BButton* save = new BButton("Save", new BMessage(MSG_SAVE_SETTINGS));
		save->MakeDefault(true);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 10)
			.SetInsets(12)
			.Add(fServer)
			.Add(fUser)
			.Add(fPassword)
			.Add(fInterface)
			.Add(fUserAgent)
			.AddGroup(B_HORIZONTAL, 8)
				.Add(fScript)
				.Add(browseScript)
			.End()
			.Add(fExtraArgs)
			.Add(fVerbose)
			.AddGroup(B_HORIZONTAL, 8)
				.AddGlue()
				.Add(cancel)
				.Add(save)
			.End();

		CenterOnScreen();
	}

	~SettingsWindow() override
	{
		delete fScriptPanel;
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case MSG_BROWSE_SCRIPT:
				ShowScriptPanel();
				break;
			case B_REFS_RECEIVED:
				HandleScriptRef(message);
				break;
			case MSG_SAVE_SETTINGS:
			{
				BMessage save(MSG_SAVE_SETTINGS);
				save.AddString("server", fServer->Text());
				save.AddString("user", fUser->Text());
				save.AddString("password", fPassword->Text());
				save.AddString("interface", fInterface->Text());
				save.AddString("useragent", fUserAgent->Text());
					save.AddString("script", fScript->Text());
					save.AddString("extra_args", fExtraArgs->Text());
					save.AddBool("verbose", fVerbose->Value() == B_CONTROL_ON);
					fOwner->PostMessage(&save);
					NotifyOwnerClosed();
					Quit();
					break;
				}
				case MSG_CANCEL_SETTINGS:
					NotifyOwnerClosed();
					Quit();
					break;
			default:
				BWindow::MessageReceived(message);
				break;
		}
	}

	bool QuitRequested() override
	{
		NotifyOwnerClosed();
		return true;
	}

private:
	void NotifyOwnerClosed()
	{
		if (fClosedNotified)
			return;
		fClosedNotified = true;
		if (fOwner != NULL)
			fOwner->PostMessage(MSG_SETTINGS_CLOSED);
	}

	void ShowScriptPanel()
	{
		if (fScriptPanel == NULL) {
			fScriptPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
				NULL, B_FILE_NODE, false);
			fScriptPanel->Window()->SetTitle("Select vpnc-script");
		}
		fScriptPanel->Show();
	}

	void HandleScriptRef(BMessage* message)
	{
		entry_ref ref;
		if (message->FindRef("refs", &ref) != B_OK)
			return;
		BEntry entry(&ref, true);
		BPath path;
		if (entry.GetPath(&path) == B_OK)
			fScript->SetText(path.Path());
	}

	BWindow* fOwner;
	BTextControl* fServer;
	BTextControl* fUser;
	BTextControl* fPassword;
	BTextControl* fInterface;
	BTextControl* fUserAgent;
	BTextControl* fScript;
	BTextControl* fExtraArgs;
	BCheckBox* fVerbose;
	BFilePanel* fScriptPanel;
	bool fClosedNotified;
};

class MainWindow : public BWindow {
public:
	MainWindow()
		:
		BWindow(BRect(80, 80, 80 + kWindowWidth, 80 + kWindowHeight), "BooConnect",
			B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS),
		fSettings(LoadSettings())
	{
		SetSizeLimits(kWindowWidth, kWindowWidth, kWindowHeight, kWindowHeight);

		BMenuBar* menuBar = new BMenuBar("menubar");
		BMenu* fileMenu = new BMenu("File");
		fileMenu->AddItem(new BMenuItem("Settings" B_UTF8_ELLIPSIS,
			new BMessage(MSG_SETTINGS), ','));
		fileMenu->AddItem(new BSeparatorItem());
		fileMenu->AddItem(new BMenuItem("Quit", new BMessage(MSG_QUIT), 'Q'));
		menuBar->AddItem(fileMenu);

		BMenu* viewMenu = new BMenu("View");
		fShowLogItem = new BMenuItem("Show Log", new BMessage(MSG_TOGGLE_LOG), 'L');
		viewMenu->AddItem(fShowLogItem);
		menuBar->AddItem(viewMenu);

		fHomeView = new BGroupView(B_VERTICAL, 12);
		fHomeView->SetExplicitMinSize(BSize(400, 340));
		fHomeView->SetExplicitPreferredSize(BSize(400, 340));
		fHomeView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 340));

		fIcon = new IconView(FindIconPath(), kIconSize);
		fTitle = new BStringView("title", "BooConnect");
		fTitle->SetExplicitAlignment(BAlignment(B_ALIGN_HORIZONTAL_CENTER, B_ALIGN_VERTICAL_CENTER));
		BFont titleFont(be_bold_font);
		titleFont.SetSize(24);
		fTitle->SetFont(&titleFont);
		fTitle->SetHighColor(kStatusNeutral);

		fStatus = new BStringView("status", "Disconnected");
		fStatus->SetExplicitAlignment(BAlignment(B_ALIGN_HORIZONTAL_CENTER, B_ALIGN_VERTICAL_CENTER));
		BFont statusFont(be_plain_font);
		statusFont.SetSize(18);
		fStatus->SetFont(&statusFont);
		fStatus->SetHighColor(kStatusRed);

		fConnectButton = new BButton("Connect", new BMessage(MSG_CONNECT));
		fConnectButton->SetExplicitMinSize(BSize(180, 46));
		fLog = new BTextView("log");
		fLog->MakeEditable(false);
		fLog->SetWordWrap(true);

		fLogScroll = new BScrollView("logscroll", fLog, 0, true, true);
		fLogScroll->SetExplicitMinSize(BSize(400, 340));
		fLogScroll->SetExplicitPreferredSize(BSize(400, 340));
		fLogScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 340));
		fLogScroll->Hide();

		BLayoutBuilder::Group<>(fHomeView, B_VERTICAL, 12)
			.SetInsets(0)
			.AddGlue()
			.AddGroup(B_HORIZONTAL, 0)
				.AddGlue()
				.Add(fIcon)
				.AddGlue()
			.End()
			.Add(fTitle)
			.Add(fStatus)
			.AddGlue();

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(menuBar)
			.AddGroup(B_VERTICAL, 12)
					.SetInsets(18, 22, 18, 20)
				.Add(fHomeView)
				.Add(fLogScroll)
				.AddGlue()
				.AddGroup(B_HORIZONTAL, 0)
					.AddGlue()
					.Add(fConnectButton)
					.AddGlue()
				.End()
			.End();

		if (!fIcon->HasIcon())
			fIcon->Hide();

		MoveTo(fSettings.windowX, fSettings.windowY);
		SetStatus("Disconnected");
		SyncExternalState();
		if (!IsConfigured(fSettings))
			PostMessage(MSG_SETTINGS);
	}

	void FrameMoved(BPoint newPosition) override
	{
		fSettings.windowX = newPosition.x;
		fSettings.windowY = newPosition.y;
		SaveSettings(fSettings);
		BWindow::FrameMoved(newPosition);
	}

	bool QuitRequested() override
	{
		if (!fReallyQuit) {
			Minimize(true);
			return false;
		}
		if (fRunning) {
			fQuitting = true;
			Disconnect();
			return false;
		}
		return true;
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case MSG_CONNECT:
				Connect();
				break;
			case MSG_DISCONNECT:
				Disconnect();
				break;
			case MSG_SETTINGS:
				ShowSettingsWindow();
				break;
			case MSG_SETTINGS_CLOSED:
				fSettingsWindow = NULL;
				break;
			case MSG_SAVE_SETTINGS:
				ApplySettings(message);
				break;
			case MSG_PROMPT_SUBMIT:
				HandlePromptSubmit(message);
				break;
			case MSG_PROMPT_CANCEL:
				AppendLog("\nPrompt cancelled by user.\n");
				Disconnect();
				break;
			case MSG_PROCESS_OUTPUT:
				HandleProcessOutput(message);
				break;
			case MSG_PROCESS_EXIT:
				HandleProcessExit(message);
				break;
			case MSG_TOGGLE_LOG:
				ToggleLog();
				break;
			case MSG_QUIT:
				RequestQuit();
				break;
			default:
				BWindow::MessageReceived(message);
				break;
		}
	}


	void RequestQuit()
	{
		fReallyQuit = true;
		if (fRunning) {
			fQuitting = true;
			Disconnect();
			return;
		}
		if (IsOpenConnectRunning()) {
			DisconnectExternal();
			snooze(300000);
		}
		be_app->PostMessage(MSG_APP_QUIT_NOW);
	}

	bool PrepareForApplicationQuit()
	{
		bool hadConnection = fRunning || IsOpenConnectRunning();
		fReallyQuit = true;
		fQuitting = true;
		fSuppressProcessMessages = true;
		if (fRunning && fPid > 0) {
			kill(fPid, SIGINT);
			snooze(1000000);
			if (fStdin >= 0)
				close(fStdin);
			if (fStdout >= 0)
				close(fStdout);
			fStdin = -1;
			fStdout = -1;
			fRunning = false;
		}
		if (IsOpenConnectRunning())
			DisconnectExternal(true);
		return hadConnection;
	}

private:
	void SetStatus(const char* status)
	{
		fStatus->SetText(status);
		std::string value = Lower(status);
		if (value.find("connected") != std::string::npos
			&& value.find("disconnect") == std::string::npos) {
			fStatus->SetHighColor(kStatusGreen);
		} else if (value.find("disconnected") != std::string::npos
			|| value.find("error") != std::string::npos) {
			fStatus->SetHighColor(kStatusRed);
		} else {
			fStatus->SetHighColor(kStatusNeutral);
		}
		fStatus->Invalidate();
	}

	void AppendLog(const std::string& text)
	{
		fLog->Insert(fLog->TextLength(), text.c_str(), (int32)text.size());
		fLog->ScrollToOffset(fLog->TextLength());
	}

	void ToggleLog()
	{
		fLogVisible = !fLogVisible;
		if (fLogVisible) {
			fHomeView->Hide();
			fLogScroll->Show();
			fShowLogItem->SetMarked(true);
			fShowLogItem->SetLabel("Hide Log");
		} else {
			fLogScroll->Hide();
			fHomeView->Show();
			fShowLogItem->SetMarked(false);
			fShowLogItem->SetLabel("Show Log");
		}
	}

	void SetConnectedUi(bool connected)
	{
		fConnectButton->SetLabel(connected ? "Disconnect" : "Connect");
		SetStatus(connected ? "Connected" : "Disconnected");
	}

	void ShowSettingsWindow()
	{
		if (fSettingsWindow != NULL && fSettingsWindow->LockLooper()) {
			if (fSettingsWindow->IsHidden())
				fSettingsWindow->Show();
			fSettingsWindow->Activate(true);
			fSettingsWindow->UnlockLooper();
			return;
		}

		fSettingsWindow = new SettingsWindow(this, fSettings);
		fSettingsWindow->Show();
	}

	void SyncExternalState()
	{
		if (fRunning)
			return;
		SetConnectedUi(IsOpenConnectRunning());
	}

	void ApplySettings(BMessage* message)
	{
		const char* value = NULL;
		if (message->FindString("server", &value) == B_OK) fSettings.server = value;
		if (message->FindString("user", &value) == B_OK) fSettings.user = value;
		if (message->FindString("password", &value) == B_OK) fSettings.password = value;
		if (message->FindString("interface", &value) == B_OK) fSettings.iface = value;
		if (message->FindString("useragent", &value) == B_OK) fSettings.userAgent = value;
		if (message->FindString("script", &value) == B_OK) fSettings.script = value;
		if (message->FindString("extra_args", &value) == B_OK) fSettings.extraArgs = value;
		bool verbose = true;
			if (message->FindBool("verbose", &verbose) == B_OK) fSettings.verbose = verbose;
			fSettings.windowX = Frame().left;
			fSettings.windowY = Frame().top;
			SaveSettings(fSettings);
			AppendLog("Settings saved to ");
		AppendLog(kSettingsPath);
		AppendLog("\n");
	}

	void Connect()
	{
		if (!IsConfigured(fSettings)) {
			SetStatus("Open Settings to configure");
			PostMessage(MSG_SETTINGS);
			return;
		}

		SyncExternalState();
		if (!fRunning && IsOpenConnectRunning()) {
			SetConnectedUi(true);
			return;
		}

		if (fRunning) {
			Disconnect();
			return;
		}

		fPromptOpen = false;
		fPasswordSent = false;
		fResponseSent = false;
		fSeen.resize(0);

		std::vector<std::string> args;
		args.push_back("openconnect");
		if (fSettings.verbose)
			args.push_back("-v");
		args.push_back("--protocol=anyconnect");
		args.push_back("--user=" + fSettings.user);
		args.push_back("--useragent=" + fSettings.userAgent);
		args.push_back("--interface=" + fSettings.iface);
		if (!Trim(fSettings.script).empty())
			args.push_back("--script=" + fSettings.script);
		for (const std::string& extra : SplitArgs(fSettings.extraArgs))
			args.push_back(extra);
		args.push_back(fSettings.server);

		AppendLog("\nStarting openconnect...\n");
		SetStatus("Connecting...");
		fConnectButton->SetLabel("Disconnect");

		if (!StartProcess(args)) {
			SetStatus("Failed to start openconnect");
			fConnectButton->SetLabel("Connect");
		}
	}

	void Disconnect()
	{
		if (!fRunning)
			return;

		AppendLog("\nDisconnecting...\n");
		SetStatus("Disconnecting...");
		fStopping = true;
		if (fPid > 0) {
			pid_t pid = fPid;
			kill(pid, SIGINT);
		}
	}

	void DisconnectExternal(bool silent = false)
	{
		if (!silent) {
			AppendLog("\nDisconnecting existing openconnect process...\n");
			SetStatus("Disconnecting...");
		}
		system("kill openconnect >/dev/null 2>&1");
		snooze(300000);
		if (!silent)
			SetConnectedUi(IsOpenConnectRunning());
	}

	bool StartProcess(const std::vector<std::string>& args)
	{
		int inPipe[2];
		int outPipe[2];
		if (pipe(inPipe) != 0 || pipe(outPipe) != 0) {
			AppendLog(std::string("pipe failed: ") + strerror(errno) + "\n");
			return false;
		}

		pid_t pid = fork();
		if (pid < 0) {
			AppendLog(std::string("fork failed: ") + strerror(errno) + "\n");
			return false;
		}

		if (pid == 0) {
			dup2(inPipe[0], STDIN_FILENO);
			dup2(outPipe[1], STDOUT_FILENO);
			dup2(outPipe[1], STDERR_FILENO);
			close(inPipe[0]);
			close(inPipe[1]);
			close(outPipe[0]);
			close(outPipe[1]);
			chdir(kBaseDir);

			std::vector<char*> argv;
			for (const std::string& arg : args)
				argv.push_back(const_cast<char*>(arg.c_str()));
			argv.push_back(NULL);
			execvp(argv[0], argv.data());
			_exit(127);
		}

		close(inPipe[0]);
		close(outPipe[1]);
		fPid = pid;
		fStdin = inPipe[1];
		fStdout = outPipe[0];
		fRunning = true;
		fStopping = false;
		fSuppressProcessMessages = false;

		fReader = std::thread([this]() { ReaderLoop(); });
		fReader.detach();
		return true;
	}

	void ReaderLoop()
	{
		char buffer[512];
		while (true) {
			ssize_t count = read(fStdout, buffer, sizeof(buffer) - 1);
			if (count <= 0)
				break;
			if (fSuppressProcessMessages)
				continue;
			buffer[count] = '\0';
			BMessage output(MSG_PROCESS_OUTPUT);
			output.AddString("text", buffer);
			PostMessage(&output);
		}

		int status = 0;
		if (fPid > 0)
			waitpid(fPid, &status, 0);

		BMessage exit(MSG_PROCESS_EXIT);
		exit.AddInt32("status", status);
		if (!fSuppressProcessMessages)
			PostMessage(&exit);
	}

	void WriteToProcess(const std::string& value)
	{
		if (fStdin < 0)
			return;
		std::string withNewline = value + "\n";
		write(fStdin, withNewline.c_str(), withNewline.size());
	}

	void RequestPrompt(const char* title, const char* label, bool hide)
	{
		if (fPromptOpen)
			return;
		fPromptOpen = true;
		(new PromptWindow(this, title, label, hide))->Show();
	}

	void HandlePromptSubmit(BMessage* message)
	{
		const char* value = "";
		message->FindString("value", &value);
		fPromptOpen = false;
		WriteToProcess(value);
		if (fCurrentPrompt == "password")
			fPasswordSent = true;
		if (fCurrentPrompt == "response")
			fResponseSent = true;
		SetStatus("Verifying...");
	}

	void HandleProcessOutput(BMessage* message)
	{
		const char* text = "";
		if (message->FindString("text", &text) != B_OK)
			return;
		std::string chunk(text);
		AppendLog(chunk);
		fSeen += chunk;
		if (fSeen.size() > 8192)
			fSeen.erase(0, fSeen.size() - 8192);

		std::string lower = Lower(fSeen);
		if (!fPasswordSent && lower.find("password:") != std::string::npos) {
			fCurrentPrompt = "password";
			if (!fSettings.password.empty()) {
				WriteToProcess(fSettings.password);
				fPasswordSent = true;
				SetStatus("Waiting for response code...");
			} else {
				SetStatus("Waiting for password...");
				RequestPrompt("BooConnect Authentication", "Password/PIN", true);
			}
			return;
		}

		if (!fResponseSent && lower.find("response:") != std::string::npos) {
			fCurrentPrompt = "response";
			SetStatus("Waiting for response code...");
			RequestPrompt("BooConnect Token", "Response Code", false);
			return;
		}

		if (lower.find("cstp connected") != std::string::npos
			|| lower.find("configured as ") != std::string::npos) {
			SetStatus("Connected");
		} else if (lower.find("failed") != std::string::npos
			|| lower.find("invalid") != std::string::npos
			|| lower.find("error") != std::string::npos) {
			SetStatus("Error");
		}
	}

	void HandleProcessExit(BMessage* message)
	{
		int32 status = 0;
		message->FindInt32("status", &status);
		if (fStdin >= 0) close(fStdin);
		if (fStdout >= 0) close(fStdout);
		fStdin = -1;
		fStdout = -1;
		fPid = -1;
		fRunning = false;
		fStopping = false;
		fPromptOpen = false;
		if (fQuitting) {
			be_app->PostMessage(MSG_APP_QUIT_NOW);
			return;
		}
		fConnectButton->SetLabel("Connect");
		SetStatus("Disconnected");
		AppendLog("\nopenconnect exited.\n");
	}

	Settings fSettings;
	BGroupView* fHomeView;
	IconView* fIcon;
	BStringView* fTitle;
	BStringView* fStatus;
	BButton* fConnectButton;
	BMenuItem* fShowLogItem;
	SettingsWindow* fSettingsWindow = NULL;
	BTextView* fLog;
	BScrollView* fLogScroll;
	pid_t fPid = -1;
	int fStdin = -1;
	int fStdout = -1;
	bool fRunning = false;
	bool fStopping = false;
	bool fQuitting = false;
	bool fReallyQuit = false;
	bool fLogVisible = false;
	bool fPromptOpen = false;
	bool fPasswordSent = false;
	bool fResponseSent = false;
	std::atomic_bool fSuppressProcessMessages = false;
	std::string fCurrentPrompt;
	std::string fSeen;
	std::thread fReader;
};

class BooConnectApp : public BApplication {
public:
	BooConnectApp()
		:
		BApplication(kAppSignature),
		fWindow(NULL),
		fAllowQuit(false)
	{
	}

	void ReadyToRun() override
	{
		fWindow = new MainWindow();
		fWindow->Show();
	}

	bool QuitRequested() override
	{
		if (fAllowQuit)
			return true;
		system("kill openconnect >/dev/null 2>&1");
		snooze(500000);
		fAllowQuit = true;
		return true;
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case MSG_APP_QUIT_NOW:
				fAllowQuit = true;
				Quit();
				break;
			default:
				BApplication::MessageReceived(message);
				break;
		}
	}

private:
	MainWindow* fWindow;
	bool fAllowQuit;
};

int
main(int argc, char** argv)
{
	DetachFromTerminalIfNeeded(argc, argv);
	chdir(kBaseDir);
	BooConnectApp app;
	app.Run();
	return 0;
}
