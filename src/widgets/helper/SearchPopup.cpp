#include "SearchPopup.hpp"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/Channel.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "messages/Message.hpp"
#include "messages/search/AuthorPredicate.hpp"
#include "messages/search/ChannelPredicate.hpp"
#include "messages/search/LinkPredicate.hpp"
#include "messages/search/MessageFlagsPredicate.hpp"
#include "messages/search/RegexPredicate.hpp"
#include "messages/search/SubstringPredicate.hpp"
#include "widgets/helper/ChannelView.hpp"

namespace chatterino {

ChannelPtr SearchPopup::filter(const QString &text, const QString &channelName,
                               const LimitedQueueSnapshot<MessagePtr> &snapshot,
                               FilterSetPtr filterSet)
{
    ChannelPtr channel(new Channel(channelName, Channel::Type::None));

    // Parse predicates from tags in "text"
    auto predicates = parsePredicates(text);

    // Check for every message whether it fulfills all predicates that have
    // been registered
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        MessagePtr message = snapshot[i];

        bool accept = true;
        for (const auto &pred : predicates)
        {
            // Discard the message as soon as one predicate fails
            if (!pred->appliesTo(*message))
            {
                accept = false;
                break;
            }
        }

        if (accept && filterSet)
            accept = filterSet->filter(message, channel);

        // If all predicates match, add the message to the channel
        if (accept)
            channel->addMessage(message);
    }

    return channel;
}

SearchPopup::SearchPopup(QWidget *parent)
    : BasePopup({}, parent)
{
    this->initLayout();
    this->resize(400, 600);
    this->addShortcuts();
}

void SearchPopup::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"search",
         [this](std::vector<QString>) -> QString {
             this->searchInput_->setFocus();
             this->searchInput_->selectAll();
             return "";
         }},
        {"delete",
         [this](std::vector<QString>) -> QString {
             this->close();
             return "";
         }},

        {"reject", nullptr},
        {"accept", nullptr},
        {"openTab", nullptr},
        {"scrollPage", nullptr},
    };

    this->shortcuts_ = getApp()->hotkeys->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

void SearchPopup::setChannelFilters(FilterSetPtr filters)
{
    this->channelFilters_ = std::move(filters);
}

void SearchPopup::setChannel(const ChannelPtr &channel)
{
    this->channelView_->setSourceChannel(channel);
    this->channelName_ = channel->getName();
    this->snapshot_ = channel->getMessageSnapshot();
    this->search();

    this->updateWindowTitle();
}

void SearchPopup::updateWindowTitle()
{
    QString historyName;

    if (this->channelName_ == "/whispers")
    {
        historyName = "whispers";
    }
    else if (this->channelName_ == "/mentions")
    {
        historyName = "mentions";
    }
    else if (this->channelName_.isEmpty())
    {
        historyName = "<empty>'s";
    }
    else
    {
        historyName = QString("%1's").arg(this->channelName_);
    }
    this->setWindowTitle("Searching in " + historyName + " history");
}

void SearchPopup::search()
{
    this->channelView_->setChannel(filter(this->searchInput_->text(),
                                          this->channelName_, this->snapshot_,
                                          this->channelFilters_));
}

void SearchPopup::initLayout()
{
    // VBOX
    {
        QVBoxLayout *layout1 = new QVBoxLayout(this);
        layout1->setMargin(0);
        layout1->setSpacing(0);

        // HBOX
        {
            QHBoxLayout *layout2 = new QHBoxLayout(this);
            layout2->setMargin(8);
            layout2->setSpacing(8);

            // SEARCH INPUT
            {
                this->searchInput_ = new QLineEdit(this);
                layout2->addWidget(this->searchInput_);

                this->searchInput_->setPlaceholderText("Type to search");
                this->searchInput_->setClearButtonEnabled(true);
                this->searchInput_->findChild<QAbstractButton *>()->setIcon(
                    QPixmap(":/buttons/clearSearch.png"));
                QObject::connect(this->searchInput_, &QLineEdit::textChanged,
                                 this, &SearchPopup::search);
            }

            layout1->addLayout(layout2);
        }

        // CHANNELVIEW
        {
            this->channelView_ = new ChannelView(this);

            layout1->addWidget(this->channelView_);
        }

        this->setLayout(layout1);
    }

    this->searchInput_->setFocus();
}

std::vector<std::unique_ptr<MessagePredicate>> SearchPopup::parsePredicates(
    const QString &input)
{
    // This regex captures all name:value predicate pairs into named capturing
    // groups and matches all other inputs seperated by spaces as normal
    // strings.
    // It also ignores whitespaces in values when being surrounded by quotation
    // marks, to enable inputs like this => regex:"kappa 123"
    static QRegularExpression predicateRegex(
        R"lit((?:(?<name>\w+):(?<value>".+?"|[^\s]+))|[^\s]+?(?=$|\s))lit");
    static QRegularExpression trimQuotationMarksRegex(R"(^"|"$)");

    QRegularExpressionMatchIterator it = predicateRegex.globalMatch(input);

    std::vector<std::unique_ptr<MessagePredicate>> predicates;
    QStringList authors;
    QStringList channels;

    while (it.hasNext())
    {
        QRegularExpressionMatch match = it.next();

        QString name = match.captured("name");

        QString value = match.captured("value");
        value.remove(trimQuotationMarksRegex);

        // match predicates
        if (name == "from")
        {
            authors.append(value);
        }
        else if (name == "has" && value == "link")
        {
            predicates.push_back(std::make_unique<LinkPredicate>());
        }
        else if (name == "in")
        {
            channels.append(value);
        }
        else if (name == "is")
        {
            predicates.push_back(
                std::make_unique<MessageFlagsPredicate>(value));
        }
        else if (name == "regex")
        {
            predicates.push_back(std::make_unique<RegexPredicate>(value));
        }
        else
        {
            predicates.push_back(
                std::make_unique<SubstringPredicate>(match.captured()));
        }
    }

    if (!authors.empty())
        predicates.push_back(std::make_unique<AuthorPredicate>(authors));

    if (!channels.empty())
        predicates.push_back(std::make_unique<ChannelPredicate>(channels));

    return predicates;
}

}  // namespace chatterino
