// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "webkit/glue/webclipboard_impl.h"

#include "base/logging.h"
#include "base/pickle.h"
#include "base/string_util.h"
#include "base/utf_string_conversions.h"
#include "googleurl/src/gurl.h"
#include "net/base/escape.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebData.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebDragData.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebImage.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebSize.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebString.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebURL.h"
#include "third_party/WebKit/Source/Platform/chromium/public/WebVector.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/custom_data_helper.h"
#include "webkit/glue/scoped_clipboard_writer_glue.h"
#include "webkit/glue/webdropdata.h"
#include "webkit/glue/webkit_glue.h"

using WebKit::WebClipboard;
using WebKit::WebData;
using WebKit::WebDragData;
using WebKit::WebImage;
using WebKit::WebString;
using WebKit::WebURL;
using WebKit::WebVector;

namespace webkit_glue {

// Static
std::string WebClipboardImpl::URLToMarkup(const WebURL& url,
    const WebString& title) {
  std::string markup("<a href=\"");
  markup.append(url.spec());
  markup.append("\">");
  // TODO(darin): HTML escape this
  markup.append(net::EscapeForHTML(UTF16ToUTF8(title)));
  markup.append("</a>");
  return markup;
}

// Static
std::string WebClipboardImpl::URLToImageMarkup(const WebURL& url,
    const WebString& title) {
  std::string markup("<img src=\"");
  markup.append(url.spec());
  markup.append("\"");
  if (!title.isEmpty()) {
    markup.append(" alt=\"");
    markup.append(net::EscapeForHTML(UTF16ToUTF8(title)));
    markup.append("\"");
  }
  markup.append("/>");
  return markup;
}

WebClipboardImpl::WebClipboardImpl(ClipboardClient* client)
    : client_(client) {
}

WebClipboardImpl::~WebClipboardImpl() {
}

uint64 WebClipboardImpl::getSequenceNumber() {
  return sequenceNumber(BufferStandard);
}

uint64 WebClipboardImpl::sequenceNumber(Buffer buffer) {
  ui::Clipboard::Buffer buffer_type;
  if (!ConvertBufferType(buffer, &buffer_type))
    return 0;

  return client_->GetSequenceNumber(buffer_type);
}

bool WebClipboardImpl::isFormatAvailable(Format format, Buffer buffer) {
  ui::Clipboard::Buffer buffer_type = ui::Clipboard::BUFFER_STANDARD;

  if (!ConvertBufferType(buffer, &buffer_type))
    return false;

  switch (format) {
    case FormatPlainText:
      return client_->IsFormatAvailable(ui::Clipboard::GetPlainTextFormatType(),
                                        buffer_type) ||
          client_->IsFormatAvailable(ui::Clipboard::GetPlainTextWFormatType(),
                                     buffer_type);
    case FormatHTML:
      return client_->IsFormatAvailable(ui::Clipboard::GetHtmlFormatType(),
                                        buffer_type);
    case FormatSmartPaste:
      return client_->IsFormatAvailable(
          ui::Clipboard::GetWebKitSmartPasteFormatType(), buffer_type);
    case FormatBookmark:
#if defined(OS_WIN) || defined(OS_MACOSX)
      return client_->IsFormatAvailable(ui::Clipboard::GetUrlWFormatType(),
                                        buffer_type);
#endif
    default:
      NOTREACHED();
  }

  return false;
}

WebVector<WebString> WebClipboardImpl::readAvailableTypes(
    Buffer buffer, bool* contains_filenames) {
  ui::Clipboard::Buffer buffer_type;
  std::vector<string16> types;
  if (ConvertBufferType(buffer, &buffer_type)) {
    client_->ReadAvailableTypes(buffer_type, &types, contains_filenames);
  }
  return types;
}

WebString WebClipboardImpl::readPlainText(Buffer buffer) {
  ui::Clipboard::Buffer buffer_type;
  if (!ConvertBufferType(buffer, &buffer_type))
    return WebString();

  if (client_->IsFormatAvailable(ui::Clipboard::GetPlainTextWFormatType(),
                                 buffer_type)) {
    string16 text;
    client_->ReadText(buffer_type, &text);
    if (!text.empty())
      return text;
  }

  if (client_->IsFormatAvailable(ui::Clipboard::GetPlainTextFormatType(),
                                 buffer_type)) {
    std::string text;
    client_->ReadAsciiText(buffer_type, &text);
    if (!text.empty())
      return ASCIIToUTF16(text);
  }

  return WebString();
}

WebString WebClipboardImpl::readHTML(Buffer buffer, WebURL* source_url,
                                     unsigned* fragment_start,
                                     unsigned* fragment_end) {
  ui::Clipboard::Buffer buffer_type;
  if (!ConvertBufferType(buffer, &buffer_type))
    return WebString();

  string16 html_stdstr;
  GURL gurl;
  client_->ReadHTML(buffer_type, &html_stdstr, &gurl,
                    static_cast<uint32*>(fragment_start),
                    static_cast<uint32*>(fragment_end));
  *source_url = gurl;
  return html_stdstr;
}

WebData WebClipboardImpl::readImage(Buffer buffer) {
  ui::Clipboard::Buffer buffer_type;
  if (!ConvertBufferType(buffer, &buffer_type))
    return WebData();

  std::string png_data;
  client_->ReadImage(buffer_type, &png_data);
  return WebData(png_data);
}

WebString WebClipboardImpl::readCustomData(Buffer buffer,
                                           const WebString& type) {
  ui::Clipboard::Buffer buffer_type;
  if (!ConvertBufferType(buffer, &buffer_type))
    return WebString();

  string16 data;
  client_->ReadCustomData(buffer_type, type, &data);
  return data;
}

void WebClipboardImpl::writeHTML(
    const WebString& html_text, const WebURL& source_url,
    const WebString& plain_text, bool write_smart_paste) {
  ScopedClipboardWriterGlue scw(client_);
  scw.WriteHTML(html_text, source_url.spec());
  scw.WriteText(plain_text);

  if (write_smart_paste)
    scw.WriteWebSmartPaste();
}

void WebClipboardImpl::writePlainText(const WebString& plain_text) {
  ScopedClipboardWriterGlue scw(client_);
  scw.WriteText(plain_text);
}

void WebClipboardImpl::writeURL(const WebURL& url, const WebString& title) {
  ScopedClipboardWriterGlue scw(client_);

  scw.WriteBookmark(title, url.spec());
  scw.WriteHTML(UTF8ToUTF16(URLToMarkup(url, title)), "");
  scw.WriteText(UTF8ToUTF16(std::string(url.spec())));
}

void WebClipboardImpl::writeImage(
    const WebImage& image, const WebURL& url, const WebString& title) {
  ScopedClipboardWriterGlue scw(client_);

  if (!image.isNull()) {
    const SkBitmap& bitmap = image.getSkBitmap();
    SkAutoLockPixels locked(bitmap);
    scw.WriteBitmapFromPixels(bitmap.getPixels(), image.size());
  }

  if (!url.isEmpty()) {
    scw.WriteBookmark(title, url.spec());
#if !defined(OS_MACOSX)
    // When writing the image, we also write the image markup so that pasting
    // into rich text editors, such as Gmail, reveals the image. We also don't
    // want to call writeText(), since some applications (WordPad) don't pick
    // the image if there is also a text format on the clipboard.
    // We also don't want to write HTML on a Mac, since Mail.app prefers to use
    // the image markup over attaching the actual image. See
    // http://crbug.com/33016 for details.
    scw.WriteHTML(UTF8ToUTF16(URLToImageMarkup(url, title)), "");
#endif
  }
}

void WebClipboardImpl::writeDataObject(const WebDragData& data) {
  ScopedClipboardWriterGlue scw(client_);

  WebDropData data_object(data);
  // TODO(dcheng): Properly support text/uri-list here.
  if (!data_object.text.is_null())
    scw.WriteText(data_object.text.string());
  if (!data_object.html.is_null())
    scw.WriteHTML(data_object.html.string(), "");
  // If there is no custom data, avoid calling WritePickledData. This ensures
  // that ScopedClipboardWriterGlue's dtor remains a no-op if the page didn't
  // modify the DataTransfer object, which is important to avoid stomping on
  // any clipboard contents written by extension functions such as
  // chrome.bookmarkManagerPrivate.copy.
  if (!data_object.custom_data.empty()) {
    Pickle pickle;
    ui::WriteCustomDataToPickle(data_object.custom_data, &pickle);
    scw.WritePickledData(pickle, ui::Clipboard::GetWebCustomDataFormatType());
  }
}

bool WebClipboardImpl::ConvertBufferType(Buffer buffer,
                                         ui::Clipboard::Buffer* result) {
  switch (buffer) {
    case BufferStandard:
      *result = ui::Clipboard::BUFFER_STANDARD;
      break;
    case BufferSelection:
#if defined(USE_X11)
#if defined(OS_CHROMEOS)
      //  Chrome OS only supports the standard clipboard,
      //  but not the X selection clipboad.
      return false;
#else
      *result = ui::Clipboard::BUFFER_SELECTION;
      break;
#endif
#endif
    default:
      NOTREACHED();
      return false;
  }
  return true;
}

}  // namespace webkit_glue
