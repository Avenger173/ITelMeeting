/****************************************************************************
** Meta object code from reading C++ file 'rtcsignaling.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../rtcsignaling.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'rtcsignaling.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.9.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN12RtcSignalingE_t {};
} // unnamed namespace

template <> constexpr inline auto RtcSignaling::qt_create_metaobjectdata<qt_meta_tag_ZN12RtcSignalingE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "RtcSignaling",
        "remoteOffer",
        "",
        "sdp",
        "remoteAnswer",
        "remoteIce",
        "ice",
        "log",
        "line",
        "connected",
        "disconnected",
        "onConnected",
        "onTextMessageReceived",
        "msg"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'remoteOffer'
        QtMocHelpers::SignalData<void(const QJsonObject &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QJsonObject, 3 },
        }}),
        // Signal 'remoteAnswer'
        QtMocHelpers::SignalData<void(const QJsonObject &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QJsonObject, 3 },
        }}),
        // Signal 'remoteIce'
        QtMocHelpers::SignalData<void(const QJsonObject &)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QJsonObject, 6 },
        }}),
        // Signal 'log'
        QtMocHelpers::SignalData<void(const QString &)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 8 },
        }}),
        // Signal 'connected'
        QtMocHelpers::SignalData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'disconnected'
        QtMocHelpers::SignalData<void()>(10, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onConnected'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'onTextMessageReceived'
        QtMocHelpers::SlotData<void(const QString &)>(12, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 13 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<RtcSignaling, qt_meta_tag_ZN12RtcSignalingE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject RtcSignaling::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12RtcSignalingE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12RtcSignalingE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN12RtcSignalingE_t>.metaTypes,
    nullptr
} };

void RtcSignaling::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<RtcSignaling *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->remoteOffer((*reinterpret_cast< std::add_pointer_t<QJsonObject>>(_a[1]))); break;
        case 1: _t->remoteAnswer((*reinterpret_cast< std::add_pointer_t<QJsonObject>>(_a[1]))); break;
        case 2: _t->remoteIce((*reinterpret_cast< std::add_pointer_t<QJsonObject>>(_a[1]))); break;
        case 3: _t->log((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 4: _t->connected(); break;
        case 5: _t->disconnected(); break;
        case 6: _t->onConnected(); break;
        case 7: _t->onTextMessageReceived((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (RtcSignaling::*)(const QJsonObject & )>(_a, &RtcSignaling::remoteOffer, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (RtcSignaling::*)(const QJsonObject & )>(_a, &RtcSignaling::remoteAnswer, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (RtcSignaling::*)(const QJsonObject & )>(_a, &RtcSignaling::remoteIce, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (RtcSignaling::*)(const QString & )>(_a, &RtcSignaling::log, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (RtcSignaling::*)()>(_a, &RtcSignaling::connected, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (RtcSignaling::*)()>(_a, &RtcSignaling::disconnected, 5))
            return;
    }
}

const QMetaObject *RtcSignaling::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *RtcSignaling::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12RtcSignalingE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int RtcSignaling::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 8)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 8;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 8)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 8;
    }
    return _id;
}

// SIGNAL 0
void RtcSignaling::remoteOffer(const QJsonObject & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void RtcSignaling::remoteAnswer(const QJsonObject & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void RtcSignaling::remoteIce(const QJsonObject & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void RtcSignaling::log(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void RtcSignaling::connected()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void RtcSignaling::disconnected()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}
QT_WARNING_POP
