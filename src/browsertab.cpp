#include "browsertab.hpp"
#include "ui_browsertab.h"
#include "mainwindow.hpp"
#include "settingsdialog.hpp"

#include "gophermaprenderer.hpp"
#include "geminirenderer.hpp"
#include "plaintextrenderer.hpp"

#include "mimeparser.hpp"

#include "certificateselectiondialog.hpp"

#include "geminiclient.hpp"
#include "webclient.hpp"
#include "gopherclient.hpp"
#include "fingerclient.hpp"
#include "abouthandler.hpp"
#include "filehandler.hpp"

#include "ioutil.hpp"
#include "kristall.hpp"

#include <cassert>
#include <QTabWidget>
#include <QMenu>
#include <QMessageBox>
#include <QInputDialog>
#include <QDockWidget>
#include <QImage>
#include <QPixmap>
#include <QFile>
#include <QMimeDatabase>
#include <QMimeType>
#include <QImageReader>
#include <QClipboard>

#include <QGraphicsPixmapItem>
#include <QGraphicsTextItem>

#include <iconv.h>

BrowserTab::BrowserTab(MainWindow *mainWindow) : QWidget(nullptr),
                                                 ui(new Ui::BrowserTab),
                                                 mainWindow(mainWindow),
                                                 current_handler(nullptr),
                                                 outline(),
                                                 graphics_scene()
{
    ui->setupUi(this);

    addProtocolHandler<GeminiClient>();
    addProtocolHandler<FingerClient>();
    addProtocolHandler<GopherClient>();
    addProtocolHandler<WebClient>();
    addProtocolHandler<AboutHandler>();
    addProtocolHandler<FileHandler>();

    this->updateUI();

    this->ui->media_browser->setVisible(false);
    this->ui->graphics_browser->setVisible(false);
    this->ui->text_browser->setVisible(true);

    this->ui->text_browser->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this->ui->url_bar, &SearchBar::escapePressed, this, &BrowserTab::on_url_bar_escapePressed);
}

BrowserTab::~BrowserTab()
{
    delete ui;
}

void BrowserTab::navigateTo(const QUrl &url, PushToHistory mode)
{
    if (mainWindow->protocols.isSchemeSupported(url.scheme()) != ProtocolSetup::Enabled)
    {
        QMessageBox::warning(this, "Kristall", "URI scheme not supported or disabled: " + url.scheme());
        return;
    }

    if ((this->current_handler != nullptr) and not this->current_handler->cancelRequest())
    {
        QMessageBox::warning(this, "Kristall", "Failed to cancel running request!");
        return;
    }

    this->redirection_count = 0;
    this->successfully_loaded = false;
    this->timer.start();

    if(not this->startRequest(url, ProtocolHandler::Default)) {
        QMessageBox::critical(this, "Kristall", QString("Failed to execute request to %1").arg(url.toString()));
        return;
    }

    if(mode == PushImmediate) {
        pushToHistory(url);
    }

    this->updateUI();
}

void BrowserTab::navigateBack(QModelIndex history_index)
{
    auto url = history.get(history_index);

    if (url.isValid())
    {
        current_history_index = history_index;
        navigateTo(url, DontPush);
    }
}

void BrowserTab::navOneBackback()
{
    navigateBack(history.oneBackward(current_history_index));
}

void BrowserTab::navOneForward()
{
    navigateBack(history.oneForward(current_history_index));
}

void BrowserTab::scrollToAnchor(QString const &anchor)
{
    qDebug() << "scroll to anchor" << anchor;
    this->ui->text_browser->scrollToAnchor(anchor);
}

void BrowserTab::reloadPage()
{
    if (current_location.isValid())
        this->navigateTo(this->current_location, DontPush);
}

void BrowserTab::toggleIsFavourite()
{
    toggleIsFavourite(not this->ui->fav_button->isChecked());
}

void BrowserTab::toggleIsFavourite(bool isFavourite)
{
    if (isFavourite)
    {
        global_favourites.add(this->current_location);
    }
    else
    {
        global_favourites.remove(this->current_location);
    }

    this->updateUI();
}

void BrowserTab::focusUrlBar()
{
    this->ui->url_bar->setFocus(Qt::ShortcutFocusReason);
    this->ui->url_bar->selectAll();
}

void BrowserTab::on_url_bar_returnPressed()
{
    QUrl url { this->ui->url_bar->text().trimmed() };

    if (url.scheme().isEmpty())
    {
        url = QUrl{"gemini://" + this->ui->url_bar->text()};
    }

    this->navigateTo(url, PushImmediate);
}

void BrowserTab::on_url_bar_escapePressed()
{
    this->ui->url_bar->setText(this->current_location.toString(QUrl::FullyEncoded));
}

void BrowserTab::on_refresh_button_clicked()
{
    reloadPage();
}

void BrowserTab::on_networkError(ProtocolHandler::NetworkError error_code, const QString &reason)
{
    QString file_name;
    switch(error_code)
    {
    case ProtocolHandler::UnknownError: file_name = "UnknownError.gemini"; break;
    case ProtocolHandler::ProtocolViolation: file_name = "ProtocolViolation.gemini"; break;
    case ProtocolHandler::HostNotFound: file_name = "HostNotFound.gemini"; break;
    case ProtocolHandler::ConnectionRefused: file_name = "ConnectionRefused.gemini"; break;
    case ProtocolHandler::ResourceNotFound: file_name = "ResourceNotFound.gemini"; break;
    case ProtocolHandler::BadRequest: file_name = "BadRequest.gemini"; break;
    case ProtocolHandler::ProxyRequest: file_name = "ProxyRequest.gemini"; break;
    case ProtocolHandler::InternalServerError: file_name = "InternalServerError.gemini"; break;
    case ProtocolHandler::InvalidClientCertificate: file_name = "InvalidClientCertificate.gemini"; break;
    case ProtocolHandler::UntrustedHost: file_name = "UntrustedHost.gemini"; break;
    case ProtocolHandler::MistrustedHost: file_name = "MistrustedHost.gemini"; break;
    case ProtocolHandler::Unauthorized: file_name = "Unauthorized.gemini"; break;
    case ProtocolHandler::TlsFailure: file_name = "TlsFailure.gemini"; break;
    case ProtocolHandler::Timeout: file_name = "Timeout.gemini"; break;
    }
    file_name = ":/error_page/" + file_name;

    QFile file_src { file_name };

    if(not file_src.open(QFile::ReadOnly)) {
        assert(false);
    }

    auto contents = QString::fromUtf8(file_src.readAll()).arg(reason).toUtf8();

    this->is_internal_location = true;

    this->on_requestComplete(
        contents,
        "text/gemini");

    this->updateUI();
}

void BrowserTab::on_certificateRequired(const QString &reason)
{
    if (not trySetClientCertificate(reason))
    {
        setErrorMessage(QString("The page requested a authorized client certificate, but none was provided.\r\nOriginal query was: %1").arg(reason));
    }
    else
    {
        this->navigateTo(this->current_location, DontPush);
    }
    this->updateUI();
}

static QByteArray convertToUtf8(QByteArray const & input, QString const & charSet)
{
    QFile temp { "/tmp/raw.dat" };
    temp.open(QFile::WriteOnly);
    IoUtil::writeAll(temp, input);

    auto charset_u8 = charSet.toUpper().toUtf8();

    // TRANSLIT will try to mix-match other code points to reflect to correct encoding
    iconv_t cd = iconv_open("UTF-8", charset_u8.data());
    if(cd == (iconv_t)-1) {
        return QByteArray { };
    }

    QByteArray result;

    char temp_buffer[4096];

    char const * input_ptr = reinterpret_cast<char const *>(input.data());
    size_t input_size = input.size();

    while(input_size > 0)
    {
        char * out_ptr = temp_buffer;
        size_t out_size = sizeof(temp_buffer);

        size_t n = iconv(cd, const_cast<char **>(&input_ptr), &input_size, &out_ptr, &out_size);
        if (n == size_t(-1))
        {
            if(errno == E2BIG) {
                // silently ignore E2BIG, as we will continue conversion in the next loop
            }
            else if(errno == EILSEQ) {
                // this is an invalid multibyte sequence.
                // append an "replacement character" and skip a byte
                if(input_size > 0) {
                    input_size --;
                    input_ptr++;
                    result.append(u8"�");
                }
            }
            else if(errno == EINVAL) {
                // the file ends with an invalid multibyte sequence.
                // just drop it and display the replacement-character
                if(input_size > 0) {
                    input_size --;
                    input_ptr++;
                    result.append(u8"�");
                }
            }
            else {
                perror("iconv conversion error");
                break;
            }
        }

        size_t len = out_ptr - temp_buffer;
        result.append(temp_buffer, len);
    }

    iconv_close(cd);

    return result;
}

void BrowserTab::on_requestComplete(const QByteArray &ref_data, const QString &mime_text)
{
    this->ui->media_browser->stopPlaying();

    QByteArray data = ref_data;
    MimeType mime = MimeParser::parse(mime_text);

    qDebug() << "Loaded" << ref_data.length() << "bytes of type" << mime.type << "/" << mime.subtype;
//    for(auto & key : mime.parameters.keys()) {
//        qDebug() << key << mime.parameters[key];
//    }

    auto charset = mime.parameter("charset", "utf-8").toUpper();
    if(not ref_data.isEmpty() and (mime.type == "text") and (charset != "UTF-8"))
    {
        auto temp = convertToUtf8(ref_data, charset);
        bool ok = (temp.size() > 0);
        if(ok) {
            data = std::move(temp);
        } else {
            auto response = QMessageBox::question(
                this,
                "Kristall",
                QString("Failed to convert input charset %1 to UTF-8. Cannot display the file.\r\nDo you want to display unconverted data anyways?").arg(charset)
            );

            if(response != QMessageBox::Yes) {
                setErrorMessage(QString("Failed to convert input charset %1 to UTF-8.").arg(charset));
                return;
            }
        }
    }

    this->current_mime = mime_text;
    this->current_buffer = ref_data;

    this->graphics_scene.clear();
    this->ui->text_browser->setText("");

    ui->text_browser->setStyleSheet("");

    enum DocumentType
    {
        Text,
        Image,
        Media
    };

    DocumentType doc_type = Text;
    std::unique_ptr<QTextDocument> document;

    this->outline.clear();

    auto doc_style = mainWindow->current_style.derive(this->current_location);

    this->ui->text_browser->setStyleSheet(QString("QTextBrowser { background-color: %1; }").arg(doc_style.background_color.name()));

    bool plaintext_only = (global_options.text_display == GenericSettings::PlainText);

    if (not plaintext_only and mime_text.startsWith("text/gemini"))
    {
        document = GeminiRenderer::render(
            data,
            this->current_location,
            doc_style,
            this->outline);
    }
    else if (not plaintext_only and mime_text.startsWith("text/gophermap"))
    {
        document = GophermapRenderer::render(
            data,
            this->current_location,
            doc_style);
    }
    else if (not plaintext_only and mime_text.startsWith("text/finger"))
    {
        document = PlainTextRenderer::render(data, doc_style);
    }
    else if (not plaintext_only and mime_text.startsWith("text/html"))
    {
        document = std::make_unique<QTextDocument>();

        document->setDefaultFont(doc_style.standard_font);
        document->setDefaultStyleSheet(doc_style.toStyleSheet());
        document->setDocumentMargin(doc_style.margin);
        document->setHtml(QString::fromUtf8(data));
    }
#if defined(QT_FEATURE_textmarkdownreader)
    else if (not plaintext_only and mime_text.startsWith("text/markdown"))
    {
        document = std::make_unique<QTextDocument>();
        document->setDefaultFont(doc_style.standard_font);
        document->setDefaultStyleSheet(doc_style.toStyleSheet());
        document->setDocumentMargin(doc_style.margin);
        document->setMarkdown(QString::fromUtf8(data));
    }
#endif
    else if (mime_text.startsWith("text/"))
    {
        document = PlainTextRenderer::render(data, doc_style);
    }
    else if (mime_text.startsWith("image/"))
    {
        doc_type = Image;

        QBuffer buffer;
        buffer.setData(data);

        QImageReader reader{&buffer};
        reader.setAutoTransform(true);
        reader.setAutoDetectImageFormat(true);

        QImage img;
        if (reader.read(&img))
        {
            auto pixmap = QPixmap::fromImage(img);
            this->graphics_scene.addPixmap(pixmap);
            this->graphics_scene.setSceneRect(pixmap.rect());
        }
        else
        {
            this->graphics_scene.addText(QString("Failed to load picture:\r\n%1").arg(reader.errorString()));
        }

        this->ui->graphics_browser->setScene(&graphics_scene);

        auto *invoker = new QObject();
        connect(invoker, &QObject::destroyed, [this]() {
            this->ui->graphics_browser->fitInView(graphics_scene.sceneRect(), Qt::KeepAspectRatio);
        });
        invoker->deleteLater();

        this->ui->graphics_browser->fitInView(graphics_scene.sceneRect(), Qt::KeepAspectRatio);
    }
    else if (mime_text.startsWith("video/") or mime_text.startsWith("audio/"))
    {
        doc_type = Media;
        this->ui->media_browser->setMedia(data, this->current_location, mime_text);
    }
    else
    {
        document = std::make_unique<QTextDocument>();
        document->setDefaultFont(doc_style.standard_font);
        document->setDefaultStyleSheet(doc_style.toStyleSheet());

        document->setPlainText(QString(R"md(You accessed an unsupported media type!

Use the *File* menu to save the file to your local disk or navigate somewhere else. I cannot display this for you. ☹

Info:
MIME Type: %1
File Size: %2
)md")
                                   .arg(mime_text)
                                   .arg(IoUtil::size_human(data.size())));
    }

    assert((document != nullptr) == (doc_type == Text));

    this->ui->text_browser->setVisible(doc_type == Text);
    this->ui->graphics_browser->setVisible(doc_type == Image);
    this->ui->media_browser->setVisible(doc_type == Media);

    this->ui->text_browser->setDocument(document.get());
    this->current_document = std::move(document);

    emit this->locationChanged(this->current_location);

    QString title = this->current_location.toString();
    emit this->titleChanged(title);

    this->current_stats.file_size = ref_data.size();
    this->current_stats.mime_type = mime;
    this->current_stats.loading_time = this->timer.elapsed();
    emit this->fileLoaded(this->current_stats);

    this->successfully_loaded = true;

    this->updateUI();
}

void BrowserTab::on_inputRequired(const QString &query)
{
    QInputDialog dialog{this};

    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setLabelText(query);

    while(true)
    {
        if (dialog.exec() != QDialog::Accepted)
        {
            setErrorMessage(QString("Site requires input:\n%1").arg(query));
            return;
        }

        QUrl new_location = current_location;
        new_location.setQuery(dialog.textValue());

        int len = new_location.toString(QUrl::FullyEncoded).toUtf8().size();
        if(len >= 1020) {
            QMessageBox::warning(
                this,
                "Kristall",
                tr("Your input message is too long. Your input is %1 bytes, but a maximum of %2 bytes are allowed.\r\nPlease cancel or shorten your input.").arg(len).arg(1020)
            );
        } else {
            this->navigateTo(new_location, DontPush);
            break;
        }
    }
}

void BrowserTab::on_redirected(const QUrl &uri, bool is_permanent)
{
    Q_UNUSED(is_permanent);

    if (redirection_count >= global_options.max_redirections)
    {
        setErrorMessage(QString("Too many consecutive redirections. The last redirection would have redirected you to:\r\n%1").arg(uri.toString(QUrl::FullyEncoded)));
        return;
    }
    else
    {
        bool is_cross_protocol = (this->current_location.scheme() != uri.scheme());
        bool is_cross_host = (this->current_location.host() != uri.host());

        QString question;
        if(global_options.redirection_policy == GenericSettings::WarnAlways)
        {
            question = QString(
                "The location you visited wants to redirect you to another location:\r\n"
                "%1\r\n"
                "Do you want to allow the redirection?"
            ).arg(uri.toString(QUrl::FullyEncoded));
        }
        else if((global_options.redirection_policy & (GenericSettings::WarnOnHostChange | GenericSettings::WarnOnSchemeChange)) and is_cross_protocol and is_cross_host)
        {
            question = QString(
                "The location you visited wants to redirect you to another host and switch the protocol.\r\n"
                "Protocol: %1\r\n"
                "New Host: %2\r\n"
                "Do you want to allow the redirection?"
            ).arg(uri.scheme()).arg(uri.host());
        }
        else if((global_options.redirection_policy & GenericSettings::WarnOnSchemeChange) and is_cross_protocol)
        {
            question = QString(
                "The location you visited wants to switch the protocol.\r\n"
                "Protocol: %1\r\n"
                "Do you want to allow the redirection?"
            ).arg(uri.scheme());
        }
        else if((global_options.redirection_policy & GenericSettings::WarnOnHostChange) and is_cross_host)
        {
            question = QString(
                "The location you visited wants to redirect you to another host.\r\n"
                "New Host: %1\r\n"
                "Do you want to allow the redirection?"
            ).arg(uri.host());
        }

        if(is_cross_protocol or is_cross_host)
        {
            auto answer = QMessageBox::question(
                this,
                "Kristall",
                question
            );
            if(answer != QMessageBox::Yes) {
                setErrorMessage(QString("Redirection to %1 cancelled by user").arg(uri.toString()));
                return;
            }
        }

        if (this->startRequest(uri, ProtocolHandler::Default))
        {
            redirection_count += 1;
            this->current_location = uri;
            this->ui->url_bar->setText(uri.toString());
        }
        else
        {
            setErrorMessage(QString("Redirection to %1 failed").arg(uri.toString()));
        }
    }
}


void BrowserTab::on_linkHovered(const QString &url)
{
    if(not url.startsWith("kristall+ctrl:"))
        this->mainWindow->setUrlPreview(QUrl(url));
}

void BrowserTab::setErrorMessage(const QString &msg)
{
    this->on_requestComplete(
        QString("An error happened:\r\n%0").arg(msg).toUtf8(),
        "text/plain charset=utf-8");

    this->updateUI();
}

void BrowserTab::pushToHistory(const QUrl &url)
{
    this->current_history_index = this->history.pushUrl(this->current_history_index, url);
    this->updateUI();
}

void BrowserTab::on_fav_button_clicked()
{
    toggleIsFavourite(this->ui->fav_button->isChecked());
}

#include <QDesktopServices>

void BrowserTab::on_text_browser_anchorClicked(const QUrl &url)
{
    qDebug() << url;

    if(url.scheme() == "kristall+ctrl")
    {
        if(this->is_internal_location) {
            QString opt = url.path();
            qDebug() << "kristall control action" << opt;
            if(opt == "ignore-tls") {
                auto response = QMessageBox::question(
                    this,
                    "Kristall",
                    tr("This sites certificate could not be verified! This may be a man-in-the-middle attack on the server to send you malicious content (or the server admin made a configuration mistake).\r\nAre you sure you want to continue?"),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No
                );
                if(response == QMessageBox::Yes) {
                    this->startRequest(this->current_location, ProtocolHandler::IgnoreTlsErrors);
                }
            }
        } else {
            QMessageBox::critical(
                this,
                "Kristall",
                tr("Malicious site detected! This site tries to use the Kristall control scheme!\r\nA trustworthy site does not do this!").arg(this->current_location.host())
            );
        }
        return;
    }

    QUrl real_url = url;
    if (real_url.isRelative())
        real_url = this->current_location.resolved(url);

    auto support = mainWindow->protocols.isSchemeSupported(real_url.scheme());

    if (support == ProtocolSetup::Enabled)
    {
        this->navigateTo(real_url, PushImmediate);
    }
    else
    {
        if (global_options.use_os_scheme_handler)
        {
            if (not QDesktopServices::openUrl(url))
            {
                QMessageBox::warning(this, "Kristall", QString("Failed to start system URL handler for\r\n%1").arg(real_url.toString()));
            }
        }
        else if (support == ProtocolSetup::Disabled)
        {
            QMessageBox::warning(this, "Kristall", QString("The requested url uses a scheme that has been disabled in the settings:\r\n%1").arg(real_url.toString()));
        }
        else
        {
            QMessageBox::warning(this, "Kristall", QString("The requested url cannot be processed by Kristall:\r\n%1").arg(real_url.toString()));
        }
    }
}

void BrowserTab::on_text_browser_highlighted(const QUrl &url)
{
    if (url.isValid())
    {
        QUrl real_url = url;
        if (real_url.isRelative())
            real_url = this->current_location.resolved(url);
        this->mainWindow->setUrlPreview(real_url);
    }
    else
    {
        this->mainWindow->setUrlPreview(QUrl{});
    }
}

void BrowserTab::on_stop_button_clicked()
{
    if(this->current_handler != nullptr) {
        this->current_handler->cancelRequest();
    }
    this->updateUI();
}

void BrowserTab::on_requestProgress(qint64 transferred)
{
    this->current_stats.file_size = transferred;
    this->current_stats.mime_type = MimeType { };
    this->current_stats.loading_time = this->timer.elapsed();
    emit this->fileLoaded(this->current_stats);
}

void BrowserTab::on_back_button_clicked()
{
    navOneBackback();
}

void BrowserTab::on_forward_button_clicked()
{
    navOneForward();
}

void BrowserTab::updateUI()
{
    this->ui->back_button->setEnabled(history.oneBackward(current_history_index).isValid());
    this->ui->forward_button->setEnabled(history.oneForward(current_history_index).isValid());

    bool in_progress = (this->current_handler != nullptr) and this->current_handler->isInProgress();

    this->ui->refresh_button->setVisible(not in_progress);
    this->ui->stop_button->setVisible(in_progress);

    this->ui->fav_button->setEnabled(this->successfully_loaded);
    this->ui->fav_button->setChecked(global_favourites.contains(this->current_location));
}

bool BrowserTab::trySetClientCertificate(const QString &query)
{
    CertificateSelectionDialog dialog{this};

    dialog.setServerQuery(query);

    if (dialog.exec() != QDialog::Accepted)
    {
        for(auto & handler : this->protocol_handlers) {
            handler->disableClientCertificate();
        }
        this->ui->enable_client_cert_button->setChecked(false);
        return false;
    }

    this->current_identitiy = dialog.identity();

    if (not current_identitiy.isValid())
    {
        QMessageBox::warning(this, "Kristall", "Failed to generate temporary crypto-identitiy");
        this->ui->enable_client_cert_button->setChecked(false);
        return false;
    }

    this->ui->enable_client_cert_button->setChecked(true);

    return true;
}

void BrowserTab::resetClientCertificate()
{
    if (this->current_identitiy.isValid() and not this->current_identitiy.is_persistent)
    {
        auto respo = QMessageBox::question(this, "Kristall", "You currently have a transient session active!\r\nIf you disable the session, you will not be able to restore it. Continue?");
        if (respo != QMessageBox::Yes)
        {
            this->ui->enable_client_cert_button->setChecked(true);
            return;
        }
    }

    this->current_identitiy = CryptoIdentity();

    for(auto & handler : this->protocol_handlers) {
        handler->disableClientCertificate();
    }
    this->ui->enable_client_cert_button->setChecked(false);
}

void BrowserTab::addProtocolHandler(std::unique_ptr<ProtocolHandler> &&handler)
{
    connect(handler.get(), &ProtocolHandler::requestProgress, this, &BrowserTab::on_requestProgress);
    connect(handler.get(), &ProtocolHandler::requestComplete, this, &BrowserTab::on_requestComplete);
    connect(handler.get(), &ProtocolHandler::redirected, this, &BrowserTab::on_redirected);
    connect(handler.get(), &ProtocolHandler::inputRequired, this, &BrowserTab::on_inputRequired);
    connect(handler.get(), &ProtocolHandler::networkError, this, &BrowserTab::on_networkError);
    connect(handler.get(), &ProtocolHandler::certificateRequired, this, &BrowserTab::on_certificateRequired);

    this->protocol_handlers.emplace_back(std::move(handler));
}

bool BrowserTab::startRequest(const QUrl &url, ProtocolHandler::RequestOptions options)
{
    this->current_handler = nullptr;
    for(auto & ptr : this->protocol_handlers)
    {
        if(ptr->supportsScheme(url.scheme())) {
            this->current_handler = ptr.get();
            break;
        }
    }

    assert((this->current_handler != nullptr) and "If this error happens, someone forgot to add a new protocol handler class in the constructor. Shame on the programmer!");

    if(this->current_identitiy.isValid()) {
        if(not this->current_handler->enableClientCertificate(this->current_identitiy)) {
            auto answer = QMessageBox::question(
                this,
                "Kristall",
                QString("You requested a %1-URL with a client certificate, but these are not supported for this scheme. Continue?").arg(url.scheme())
            );
            if(answer != QMessageBox::Yes)
                return false;
            this->current_handler->disableClientCertificate();
            this->ui->enable_client_cert_button->setChecked(false);
        }
    } else {
        this->current_handler->disableClientCertificate();
        this->ui->enable_client_cert_button->setChecked(false);
    }

    if(this->current_identitiy.isValid() and (url.host() != this->current_location.host())) {
        auto answer = QMessageBox::question(
            this,
            "Kristall",
            "You want to visit a new host, but have a client certificate enabled. This may be a risk to expose your identity to another host.\r\nDo you want to keep the certificate enabled?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if(answer != QMessageBox::Yes) {
            this->current_handler->disableClientCertificate();
            this->ui->enable_client_cert_button->setChecked(false);
        }
    }

    this->is_internal_location = (url.scheme() == "about");
    this->current_location = url;
    this->ui->url_bar->setText(url.toString(QUrl::FormattingOptions(QUrl::FullyEncoded)));

    return this->current_handler->startRequest(url, options);
}

void BrowserTab::on_text_browser_customContextMenuRequested(const QPoint &pos)
{
    QMenu menu;

    QString anchor = ui->text_browser->anchorAt(pos);
    if (not anchor.isEmpty())
    {
        QUrl real_url{anchor};
        if (real_url.isRelative())
            real_url = this->current_location.resolved(real_url);

        connect(menu.addAction("Follow link…"), &QAction::triggered, [this, real_url]() {
            this->navigateTo(real_url, PushImmediate);
        });

        connect(menu.addAction("Open in new tab…"), &QAction::triggered, [this, real_url]() {
            mainWindow->addNewTab(false, real_url);
        });

        connect(menu.addAction("Copy link"), &QAction::triggered, [real_url]() {
            global_clipboard->setText(real_url.toString(QUrl::FullyEncoded));
        });

        menu.addSeparator();
    }

    connect(menu.addAction("Select all"), &QAction::triggered, [this]() {
        this->ui->text_browser->selectAll();
    });

    menu.addSeparator();

    QAction * copy = menu.addAction("Copy to clipboard");
    copy->setShortcut(QKeySequence("Ctrl-C"));
    connect(copy, &QAction::triggered, [this]() {
        this->ui->text_browser->copy();
    });

    menu.exec(ui->text_browser->mapToGlobal(pos));
}

void BrowserTab::on_enable_client_cert_button_clicked(bool checked)
{
    if (checked)
    {
        trySetClientCertificate(QString{});
    }
    else
    {
        resetClientCertificate();
    }
}
