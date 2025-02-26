#include "messages/SharedMessageBuilder.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "controllers/ignores/IgnoreController.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/StreamerMode.hpp"

#include <QFileInfo>
#include <QMediaPlayer>

namespace chatterino {

namespace {

    QUrl getFallbackHighlightSound()
    {
        QString path = getSettings()->pathHighlightSound;
        bool fileExists = QFileInfo::exists(path) && QFileInfo(path).isFile();

        // Use fallback sound when checkbox is not checked
        // or custom file doesn't exist
        if (getSettings()->customHighlightSound && fileExists)
        {
            return QUrl::fromLocalFile(path);
        }
        else
        {
            return QUrl("qrc:/sounds/ping2.wav");
        }
    }

    QStringList parseTagList(const QVariantMap &tags, const QString &key)
    {
        auto iterator = tags.find(key);
        if (iterator == tags.end())
            return QStringList{};

        return iterator.value().toString().split(
            ',', QString::SplitBehavior::SkipEmptyParts);
    }

    std::vector<Badge> parseBadges(const QVariantMap &tags)
    {
        std::vector<Badge> badges;

        for (QString badge : parseTagList(tags, "badges"))
        {
            QStringList parts = badge.split('/');
            if (parts.size() != 2)
            {
                continue;
            }

            badges.emplace_back(parts[0], parts[1]);
        }

        return badges;
    }

}  // namespace

SharedMessageBuilder::SharedMessageBuilder(
    Channel *_channel, const Communi::IrcPrivateMessage *_ircMessage,
    const MessageParseArgs &_args)
    : channel(_channel)
    , ircMessage(_ircMessage)
    , args(_args)
    , tags(this->ircMessage->tags())
    , originalMessage_(_ircMessage->content())
    , action_(_ircMessage->isAction())
{
}

SharedMessageBuilder::SharedMessageBuilder(
    Channel *_channel, const Communi::IrcMessage *_ircMessage,
    const MessageParseArgs &_args, QString content, bool isAction)
    : channel(_channel)
    , ircMessage(_ircMessage)
    , args(_args)
    , tags(this->ircMessage->tags())
    , originalMessage_(content)
    , action_(isAction)
{
}

void SharedMessageBuilder::parse()
{
    this->parseUsernameColor();

    if (this->action_)
    {
        this->textColor_ = this->usernameColor_;
    }

    this->parseUsername();

    this->message().flags.set(MessageFlag::Collapsed);
}

bool SharedMessageBuilder::isIgnored() const
{
    return isIgnoredMessage({
        /*.message = */ this->originalMessage_,
    });
}

void SharedMessageBuilder::parseUsernameColor()
{
    if (getSettings()->colorizeNicknames)
    {
        this->usernameColor_ = getRandomColor(this->ircMessage->nick());
    }
}

void SharedMessageBuilder::parseUsername()
{
    // username
    this->userName = this->ircMessage->nick();

    this->message().loginName = this->userName;
}

void SharedMessageBuilder::parseHighlights()
{
    auto app = getApp();

    // Highlight because it's a subscription
    if (this->message().flags.has(MessageFlag::Subscription) &&
        getSettings()->enableSubHighlight)
    {
        if (getSettings()->enableSubHighlightTaskbar)
        {
            this->highlightAlert_ = true;
        }

        if (getSettings()->enableSubHighlightSound)
        {
            this->highlightSound_ = true;

            // Use custom sound if set, otherwise use fallback
            if (!getSettings()->subHighlightSoundUrl.getValue().isEmpty())
            {
                this->highlightSoundUrl_ =
                    QUrl(getSettings()->subHighlightSoundUrl.getValue());
            }
            else
            {
                this->highlightSoundUrl_ = getFallbackHighlightSound();
            }
        }

        this->message().flags.set(MessageFlag::Highlighted);
        this->message().highlightColor =
            ColorProvider::instance().color(ColorType::Subscription);
    }

    // XXX: Non-common term in SharedMessageBuilder
    auto currentUser = app->accounts->twitch.getCurrent();

    QString currentUsername = currentUser->getUserName();

    if (getCSettings().isBlacklistedUser(this->ircMessage->nick()))
    {
        // Do nothing. We ignore highlights from this user.
        return;
    }

    // Highlight because it's a whisper
    if (this->args.isReceivedWhisper && getSettings()->enableWhisperHighlight)
    {
        if (getSettings()->enableWhisperHighlightTaskbar)
        {
            this->highlightAlert_ = true;
        }

        if (getSettings()->enableWhisperHighlightSound)
        {
            this->highlightSound_ = true;

            // Use custom sound if set, otherwise use fallback
            if (!getSettings()->whisperHighlightSoundUrl.getValue().isEmpty())
            {
                this->highlightSoundUrl_ =
                    QUrl(getSettings()->whisperHighlightSoundUrl.getValue());
            }
            else
            {
                this->highlightSoundUrl_ = getFallbackHighlightSound();
            }
        }

        this->message().highlightColor =
            ColorProvider::instance().color(ColorType::Whisper);

        /*
         * Do _NOT_ return yet, we might want to apply phrase/user name
         * highlights (which override whisper color/sound).
         */
    }

    // Highlight because of sender
    auto userHighlights = getCSettings().highlightedUsers.readOnly();
    for (const HighlightPhrase &userHighlight : *userHighlights)
    {
        if (!userHighlight.isMatch(this->ircMessage->nick()))
        {
            continue;
        }
        qCDebug(chatterinoMessage)
            << "Highlight because user" << this->ircMessage->nick()
            << "sent a message";

        this->message().flags.set(MessageFlag::Highlighted);
        if (!(this->message().flags.has(MessageFlag::Subscription) &&
              getSettings()->enableSubHighlight))
        {
            this->message().highlightColor = userHighlight.getColor();
        }

        if (userHighlight.showInMentions())
        {
            this->message().flags.set(MessageFlag::ShowInMentions);
        }

        if (userHighlight.hasAlert())
        {
            this->highlightAlert_ = true;
        }

        if (userHighlight.hasSound())
        {
            this->highlightSound_ = true;
            // Use custom sound if set, otherwise use the fallback sound
            if (userHighlight.hasCustomSound())
            {
                this->highlightSoundUrl_ = userHighlight.getSoundUrl();
            }
            else
            {
                this->highlightSoundUrl_ = getFallbackHighlightSound();
            }
        }

        if (this->highlightAlert_ && this->highlightSound_)
        {
            /*
             * User name highlights "beat" highlight phrases: If a message has
             * all attributes (color, taskbar flashing, sound) set, highlight
             * phrases will not be checked.
             */
            return;
        }
    }

    if (this->ircMessage->nick() == currentUsername)
    {
        // Do nothing. Highlights cannot be triggered by yourself
        return;
    }

    // TODO: This vector should only be rebuilt upon highlights being changed
    // fourtf: should be implemented in the HighlightsController
    std::vector<HighlightPhrase> activeHighlights =
        getSettings()->highlightedMessages.cloneVector();

    if (!currentUser->isAnon() && getSettings()->enableSelfHighlight &&
        currentUsername.size() > 0)
    {
        HighlightPhrase selfHighlight(
            currentUsername, getSettings()->showSelfHighlightInMentions,
            getSettings()->enableSelfHighlightTaskbar,
            getSettings()->enableSelfHighlightSound, false, false,
            getSettings()->selfHighlightSoundUrl.getValue(),
            ColorProvider::instance().color(ColorType::SelfHighlight));
        activeHighlights.emplace_back(std::move(selfHighlight));
    }

    // Highlight because of message
    for (const HighlightPhrase &highlight : activeHighlights)
    {
        if (!highlight.isMatch(this->originalMessage_))
        {
            continue;
        }

        this->message().flags.set(MessageFlag::Highlighted);
        if (!(this->message().flags.has(MessageFlag::Subscription) &&
              getSettings()->enableSubHighlight))
        {
            this->message().highlightColor = highlight.getColor();
        }

        if (highlight.showInMentions())
        {
            this->message().flags.set(MessageFlag::ShowInMentions);
        }

        if (highlight.hasAlert())
        {
            this->highlightAlert_ = true;
        }

        // Only set highlightSound_ if it hasn't been set by username
        // highlights already.
        if (highlight.hasSound() && !this->highlightSound_)
        {
            this->highlightSound_ = true;

            // Use custom sound if set, otherwise use fallback sound
            if (highlight.hasCustomSound())
            {
                this->highlightSoundUrl_ = highlight.getSoundUrl();
            }
            else
            {
                this->highlightSoundUrl_ = getFallbackHighlightSound();
            }
        }

        if (this->highlightAlert_ && this->highlightSound_)
        {
            /*
             * Break once no further attributes (taskbar, sound) can be
             * applied.
             */
            break;
        }
    }

    // Highlight because of badge
    auto badges = parseBadges(this->tags);
    auto badgeHighlights = getCSettings().highlightedBadges.readOnly();
    bool badgeHighlightSet = false;
    for (const HighlightBadge &highlight : *badgeHighlights)
    {
        for (const Badge &badge : badges)
        {
            if (!highlight.isMatch(badge))
            {
                continue;
            }

            if (!badgeHighlightSet)
            {
                this->message().flags.set(MessageFlag::Highlighted);
                if (!(this->message().flags.has(MessageFlag::Subscription) &&
                      getSettings()->enableSubHighlight))
                {
                    this->message().highlightColor = highlight.getColor();
                }

                badgeHighlightSet = true;
            }

            if (highlight.hasAlert())
            {
                this->highlightAlert_ = true;
            }

            // Only set highlightSound_ if it hasn't been set by badge
            // highlights already.
            if (highlight.hasSound() && !this->highlightSound_)
            {
                this->highlightSound_ = true;
                // Use custom sound if set, otherwise use fallback sound
                this->highlightSoundUrl_ = highlight.hasCustomSound()
                                               ? highlight.getSoundUrl()
                                               : getFallbackHighlightSound();
            }

            if (this->highlightAlert_ && this->highlightSound_)
            {
                /*
                 * Break once no further attributes (taskbar, sound) can be
                 * applied.
                 */
                break;
            }
        }
    }
}

void SharedMessageBuilder::addTextOrEmoji(EmotePtr emote)
{
    this->emplace<EmoteElement>(emote, MessageElementFlag::EmojiAll);
}

void SharedMessageBuilder::addTextOrEmoji(const QString &string_)
{
    auto string = QString(string_);

    // Actually just text
    auto linkString = this->matchLink(string);
    auto link = Link();
    auto &&textColor = this->textColor_;

    if (linkString.isEmpty())
    {
        if (string.startsWith('@'))
        {
            this->emplace<TextElement>(string, MessageElementFlag::BoldUsername,
                                       textColor, FontStyle::ChatMediumBold);
            this->emplace<TextElement>(
                string, MessageElementFlag::NonBoldUsername, textColor);
        }
        else
        {
            this->emplace<TextElement>(string, MessageElementFlag::Text,
                                       textColor);
        }
    }
    else
    {
        this->addLink(string, linkString);
    }
}

void SharedMessageBuilder::appendChannelName()
{
    QString channelName("#" + this->channel->getName());
    Link link(Link::JumpToChannel, this->channel->getName());

    this->emplace<TextElement>(channelName, MessageElementFlag::ChannelName,
                               MessageColor::System)
        ->setLink(link);
}

inline QMediaPlayer *getPlayer()
{
    if (isGuiThread())
    {
        static auto player = new QMediaPlayer;
        return player;
    }
    else
    {
        return nullptr;
    }
}

void SharedMessageBuilder::triggerHighlights()
{
    static QUrl currentPlayerUrl;

    if (isInStreamerMode() && getSettings()->streamerModeMuteMentions)
    {
        // We are in streamer mode with muting mention sounds enabled. Do nothing.
        return;
    }

    if (getCSettings().isMutedChannel(this->channel->getName()))
    {
        // Do nothing. Pings are muted in this channel.
        return;
    }

    bool hasFocus = (QApplication::focusWidget() != nullptr);
    bool resolveFocus = !hasFocus || getSettings()->highlightAlwaysPlaySound;

    if (this->highlightSound_ && resolveFocus)
    {
        if (auto player = getPlayer())
        {
            // update the media player url if necessary
            if (currentPlayerUrl != this->highlightSoundUrl_)
            {
                player->setMedia(this->highlightSoundUrl_);

                currentPlayerUrl = this->highlightSoundUrl_;
            }

            player->play();
        }
    }

    if (this->highlightAlert_)
    {
        getApp()->windows->sendAlert();
    }
}

}  // namespace chatterino
