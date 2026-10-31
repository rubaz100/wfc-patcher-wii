#pragma once

#if RMC

#  include "MessagePopupPage.hpp"
#  include "import/mkw/ui/section/SectionManager.hpp"
#  include "wwfcLibC.hpp"

namespace wwfc::mkw::UI
{

class WifiMenuPage : public Page
{
public:
    void showMessageOfTheDay()
    {
        if (s_hasQueuedNotice) {
            if (pushMessagePopup(s_queuedNotice)) {
                s_hasQueuedNotice = false;
            }
            return;
        }
        if (s_hasSeenMessageOfTheDay) {
            return;
        }

        if (pushMessagePopup(s_messageOfTheDay)) {
            s_hasSeenMessageOfTheDay = true;
        }
    }

    static void QueueNotice(const wchar_t* message)
    {
        std::size_t i = 0;
        for (; i + 1 < std::size(s_queuedNotice) && message[i]; ++i) {
            s_queuedNotice[i] = message[i];
        }
        s_queuedNotice[i] = L'\0';
        s_hasQueuedNotice = true;
    }

    static void SetMessageOfTheDay(wchar_t* messageOfTheDay)
    {
        s_messageOfTheDay = messageOfTheDay;
    }

    static wchar_t* MessageOfTheDayBuffer()
    {
        return s_messageOfTheDayBuffer;
    }

    static std::size_t MessageOfTheDayBufferSize()
    {
        return sizeof(s_messageOfTheDayBuffer);
    }

private:
    bool pushMessagePopup(const wchar_t* message)
    {
        auto* manager = SectionManager::Instance();
        if (!manager || !manager->currentSection()) {
            return false;
        }
        Section*          section = manager->currentSection();
        MessagePopupPage* messagePopupPage =
            section->getPage<MessagePopupPage>(EPageID::POPUP_MESSAGE);
        if (!messagePopupPage) {
            return false;
        }

        FormatParam formatParam{
            .strings = { message },
        };
        messagePopupPage->reset();
        messagePopupPage->setWindowMessage(0x19CA, &formatParam);

        push(EPageID::POPUP_MESSAGE, ENextType::FORWARD);
        return true;
    }

    /* 0x044 */ u8 _044[0xF34 - 0x044];

    static const wchar_t* s_messageOfTheDay;
    static wchar_t        s_messageOfTheDayBuffer[256];
    static bool           s_hasSeenMessageOfTheDay;
    static wchar_t        s_queuedNotice[128];
    static bool           s_hasQueuedNotice;
};

static_assert(sizeof(WifiMenuPage) == 0xF34);

inline void WifiMenuPage_showMessageOfTheDay(mkw::UI::WifiMenuPage* wifiMenuPage)
{
    wifiMenuPage->showMessageOfTheDay();
}

} // namespace wwfc::mkw::UI

#endif // RMC
