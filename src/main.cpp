#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

using namespace geode::prelude;

// ======================= State =======================
using Clock = std::chrono::steady_clock;

struct PendingInput {
	Clock::time_point due;
	bool down;
	int button;
	bool isPlayer1;
};

static float g_tps = 240.f;
static float g_fps = 60.f;
static int g_inputDelayMs = 0;      // effective delay (0..10000)
static bool g_extrapolate = false;

static std::deque<PendingInput> g_queue;
static GJBaseGameLayer* g_queueOwner = nullptr;

static float g_extrapOffset = 0.f;
static bool g_extrapApplied = false;

static void applyFPS(float fps) {
	g_fps = std::clamp(fps, 1.f, 1000.f);
	CCDirector::sharedDirector()->setAnimationInterval(1.0 / g_fps);
	Mod::get()->setSavedValue<float>("fps", g_fps);
}

static void applyTPS(float tps) {
	g_tps = std::clamp(tps, 1.f, 100000.f);
	Mod::get()->setSavedValue<float>("tps", g_tps);
}

$on_mod(Loaded) {
	g_tps = Mod::get()->getSavedValue<float>("tps", 240.f);
	g_fps = Mod::get()->getSavedValue<float>("fps", 60.f);
	g_inputDelayMs = Mod::get()->getSavedValue<int>("input-delay", 0);
	g_extrapolate = Mod::get()->getSavedValue<bool>("extrapolate", false);
}

// ======================= Game hooks =======================
class $modify(ChangerBaseLayer, GJBaseGameLayer) {
	bool init() {
		g_queue.clear();
		g_queueOwner = nullptr;
		g_extrapApplied = false;
		return GJBaseGameLayer::init();
	}

	// --- TPS: replace the fixed 1/240 physics step with 1/TPS.
	// Reconstructed from the 2.2 decompilation; check member names against
	// your Geode bindings if this fails to compile.
	float getModifiedDelta(float dt) {
		if (std::abs(g_tps - 240.f) < 0.001f) {
			return GJBaseGameLayer::getModifiedDelta(dt);
		}
		float modifier = std::min(1.f, m_gameState.m_timeWarp) / g_tps;
		float total = dt + m_extraDelta;
		double steps = std::max(1.0, std::round(total / modifier));
		float newDelta = static_cast<float>(steps) * modifier;
		m_extraDelta = total - newDelta;
		return newDelta;
	}

	// --- Input delay: queue presses/releases and replay them later.
	void handleButton(bool down, int button, bool isPlayer1) {
		if (g_inputDelayMs <= 0) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}
		if (g_queueOwner != this) {
			g_queue.clear();
			g_queueOwner = this;
		}
		g_queue.push_back({
			Clock::now() + std::chrono::milliseconds(g_inputDelayMs),
			down, button, isPlayer1
		});
	}

	void update(float dt) {
		// Undo last frame's extrapolation before the game reads positions.
		if (g_extrapApplied && m_objectLayer) {
			m_objectLayer->setPositionX(m_objectLayer->getPositionX() + g_extrapOffset);
		}
		g_extrapApplied = false;

		// Fire any delayed inputs that are now due.
		if (g_queueOwner == this) {
			auto now = Clock::now();
			while (!g_queue.empty() && g_queue.front().due <= now) {
				auto e = g_queue.front();
				g_queue.pop_front();
				GJBaseGameLayer::handleButton(e.down, e.button, e.isPlayer1);
			}
		}

		GJBaseGameLayer::update(dt);

		// --- Frame extrapolation (experimental, classic mode, X axis only).
		// Physics runs in fixed ticks; the leftover time (m_extraDelta) is how far
		// the real clock is past the last tick. We shift the world by
		// speed * leftover so motion looks smooth when FPS > TPS.
		if (g_extrapolate && !m_isEditor && m_player1 && m_objectLayer
			&& m_levelSettings && !m_levelSettings->m_platformerMode) {
			float extra = std::clamp(m_extraDelta, -0.05f, 0.05f);
			float vx = m_player1->m_playerSpeed * 346.2f * m_gameState.m_timeWarp;
			g_extrapOffset = vx * extra;
			m_objectLayer->setPositionX(m_objectLayer->getPositionX() - g_extrapOffset);
			g_extrapApplied = true;
		}
	}
};

// ======================= GUI =======================
class ChangerPopup : public geode::Popup {
protected:
	TextInput* m_tpsInput = nullptr;
	TextInput* m_fpsInput = nullptr;
	TextInput* m_delayInput = nullptr;

	CCMenuItemSpriteExtra* makeButton(char const* text, SEL_MenuHandler cb, CCPoint pos, CCMenu* menu) {
		auto btn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create(text, "bigFont.fnt", "GJ_button_01.png", 0.6f),
			this, cb);
		btn->setPosition(pos);
		menu->addChild(btn);
		return btn;
	}

	TextInput* makeInput(char const* placeholder, std::string const& value, std::string const& filter, CCPoint pos) {
		auto in = TextInput::create(120.f, placeholder, "bigFont.fnt");
		in->setFilter(filter);
		in->setString(value);
		in->setPosition(pos);
		m_mainLayer->addChild(in);
		return in;
	}

	// Small round "i" button that opens a short description of a feature.
	void addInfo(CCMenu* menu, std::string title, std::string text, CCPoint pos) {
		auto spr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
		spr->setScale(0.5f);
		auto btn = CCMenuItemExt::createSpriteExtra(spr, [title, text](CCObject*) {
			FLAlertLayer::create(title.c_str(), text, "OK")->show();
		});
		btn->setPosition(pos);
		menu->addChild(btn);
	}

	bool init() {
		if (!Popup::init(300.f, 230.f)) return false;
		this->setTitle("TPS / FPS Changer");
		auto size = m_mainLayer->getContentSize();
		float cx = size.width / 2.f;
		float cy = size.height / 2.f;

		auto menu = CCMenu::create();
		menu->setPosition({0.f, 0.f});
		m_mainLayer->addChild(menu);

		float inX = cx - 55.f;
		float btnX = cx + 80.f;

		// TPS row
		m_tpsInput = makeInput("TPS", fmt::format("{:.0f}", g_tps), "0123456789.", {inX, cy + 55.f});
		makeButton("Set TPS", menu_selector(ChangerPopup::onSetTPS), {btnX, cy + 55.f}, menu);

		// FPS row
		m_fpsInput = makeInput("FPS", fmt::format("{:.0f}", g_fps), "0123456789.", {inX, cy + 15.f});
		makeButton("Set FPS", menu_selector(ChangerPopup::onSetFPS), {btnX, cy + 15.f}, menu);

		// Input delay row
		m_delayInput = makeInput("Delay (ms)", std::to_string(g_inputDelayMs), "-0123456789", {inX, cy - 25.f});
		makeButton("Set Delay", menu_selector(ChangerPopup::onSetDelay), {btnX, cy - 25.f}, menu);

		// Frame extrapolation toggle
		auto toggle = CCMenuItemExt::createTogglerWithStandardSprites(0.7f, [](CCMenuItemToggler* t) {
			// callback runs before the visual flip, so the new state is the inverse
			g_extrapolate = !t->isToggled();
			Mod::get()->setSavedValue<bool>("extrapolate", g_extrapolate);
		});
		toggle->toggle(g_extrapolate);
		toggle->setPosition({35.f, cy - 65.f});
		menu->addChild(toggle);

		auto label = CCLabelBMFont::create("Frame Extrapolation", "bigFont.fnt");
		label->setScale(0.5f);
		label->setAnchorPoint({0.f, 0.5f});
		label->setPosition({60.f, cy - 65.f});
		m_mainLayer->addChild(label);

		// Info buttons, top-right corner of each feature row
		float infoX = size.width - 18.f;
		addInfo(menu, "TPS Changer",
			"Sets how many physics ticks run per second. The default is 240. "
			"Other values change how the game plays, so it counts as a bypass.",
			{infoX, cy + 55.f + 16.f});
		addInfo(menu, "FPS Changer",
			"Sets the frame rate cap. Higher values look smoother, but your "
			"screen's refresh rate may limit what you actually see.",
			{infoX, cy + 15.f + 16.f});
		addInfo(menu, "Input Delay",
			"Delays your taps by this many milliseconds (0 to 10000). "
			"Negative values can't be applied and are treated as 0.",
			{infoX, cy - 25.f + 16.f});
		addInfo(menu, "Frame Extrapolation",
			"Experimental. Predicts movement between physics ticks to look "
			"smoother when FPS is higher than TPS. Classic mode only.",
			{infoX, cy - 65.f + 14.f});

		return true;
	}

	void onSetTPS(CCObject*) {
		auto v = numFromString<float>(m_tpsInput->getString());
		if (v.isOk() && v.unwrap() > 0.f) {
			applyTPS(v.unwrap());
			Notification::create(fmt::format("TPS set to {:.0f}", g_tps), NotificationIcon::Success)->show();
		}
	}

	void onSetFPS(CCObject*) {
		auto v = numFromString<float>(m_fpsInput->getString());
		if (v.isOk() && v.unwrap() > 0.f) {
			applyFPS(v.unwrap());
			Notification::create(fmt::format("FPS set to {:.0f}", g_fps), NotificationIcon::Success)->show();
		}
	}

	void onSetDelay(CCObject*) {
		auto v = numFromString<int>(m_delayInput->getString());
		if (!v.isOk()) return;
		int requested = std::clamp(v.unwrap(), -10000, 10000);
		if (requested < 0) {
			// A click can't be processed before it happens, so negative values
			// are treated as 0 ms.
			g_inputDelayMs = 0;
			m_delayInput->setString("0");
			Notification::create("Negative delay can't be applied, using 0 ms", NotificationIcon::Warning)->show();
		} else {
			g_inputDelayMs = requested;
			Notification::create(fmt::format("Input delay set to {} ms", g_inputDelayMs), NotificationIcon::Success)->show();
		}
		Mod::get()->setSavedValue<int>("input-delay", g_inputDelayMs);
	}

public:
	static ChangerPopup* create() {
		auto ret = new ChangerPopup();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

// ======================= Buttons that open the popup =======================
class $modify(ChangerMenuLayer, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;

		applyFPS(g_fps); // re-apply saved FPS on startup

		auto btn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("TPS/FPS", "bigFont.fnt", "GJ_button_01.png", 0.5f),
			this, menu_selector(ChangerMenuLayer::onOpenChanger));
		btn->setID("tps-fps-button"_spr);

		if (auto menu = this->getChildByID("bottom-menu")) {
			menu->addChild(btn);
			menu->updateLayout();
		}
		return true;
	}

	void onOpenChanger(CCObject*) {
		if (auto p = ChangerPopup::create()) p->show();
	}
};

class $modify(ChangerPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto btn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("TPS/FPS", "bigFont.fnt", "GJ_button_01.png", 0.4f),
			this, menu_selector(ChangerPauseLayer::onOpenChanger));
		btn->setID("tps-fps-pause-button"_spr);

		if (auto menu = this->getChildByID("right-button-menu")) {
			menu->addChild(btn);
			menu->updateLayout();
		}
	}

	void onOpenChanger(CCObject*) {
		if (auto p = ChangerPopup::create()) p->show();
	}
};
