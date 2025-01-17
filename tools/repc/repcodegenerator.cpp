/****************************************************************************
**
** Copyright (C) 2017-2020 Ford Motor Company
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtRemoteObjects module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "repcodegenerator.h"

#include "repparser.h"

#include <QFileInfo>
#include <QMetaType>
#include <QTextStream>
#include <QCryptographicHash>
#include <QRegularExpression>

using namespace Qt;

QT_BEGIN_NAMESPACE

template <typename C>
static int accumulatedSizeOfNames(const C &c)
{
    int result = 0;
    for (const auto &e : c)
        result += e.name.size();
    return result;
}

template <typename C>
static int accumulatedSizeOfTypes(const C &c)
{
    int result = 0;
    for (const auto &e : c)
        result += e.type.size();
    return result;
}

static QString cap(QString name)
{
    if (!name.isEmpty())
        name[0] = name[0].toUpper();
    return name;
}

static bool isClassEnum(const ASTClass &classContext, const QString &typeName)
{
    for (const ASTEnum &astEnum : classContext.enums) {
        if (astEnum.name == typeName) {
            return true;
        }
    }

    return false;
}

static QString fullyQualifiedTypeName(const ASTClass& classContext, const QString &className, const QString &typeName)
{
    if (isClassEnum(classContext, typeName)) {
        // type was defined in this class' context, prefix typeName with class name
        return className + QStringLiteral("::") + typeName;
    }
    return typeName;
}

// for enums we need to transform signal/slot arguments to include the class scope
static QList<ASTFunction> transformEnumParams(const ASTClass& classContext, const QList<ASTFunction> &methodList, const QString &typeName) {
    QList<ASTFunction> localList = methodList;
    for (ASTFunction &astFunction : localList) {
        for (ASTDeclaration &astParam : astFunction.params) {
            for (const ASTEnum &astEnum : classContext.enums) {
                if (astEnum.name == astParam.type) {
                    astParam.type = typeName + QStringLiteral("::") + astParam.type;
                }
            }
        }
    }
    return localList;
}

/*
  Returns \c true if the type is a built-in type.
*/
static bool isBuiltinType(const QString &type)
 {
    const auto metaType = QMetaType::fromName(type.toLatin1().constData());
    if (!metaType.isValid())
        return false;
    return (metaType.id() < QMetaType::User);
}

RepCodeGenerator::RepCodeGenerator(QIODevice *outputDevice)
    : m_outputDevice(outputDevice)
{
    Q_ASSERT(m_outputDevice);
}

static QByteArray enumSignature(const ASTEnum &e)
{
    QByteArray ret;
    ret += e.name.toLatin1();
    for (const ASTEnumParam &param : e.params)
        ret += param.name.toLatin1() + QByteArray::number(param.value);
    return ret;
}

static QByteArray typeData(const QString &type, const QHash<QString, QByteArray> &specialTypes)
{
    QHash<QString, QByteArray>::const_iterator it = specialTypes.find(type);
    if (it != specialTypes.end())
        return it.value();
    const auto pos = type.lastIndexOf(QLatin1String("::"));
    if (pos > 0)
            return typeData(type.mid(pos + 2), specialTypes);
    return type.toLatin1();
}

static QByteArray functionsData(const QList<ASTFunction> &functions, const QHash<QString, QByteArray> &specialTypes)
{
    QByteArray ret;
    for (const ASTFunction &func : functions) {
        ret += func.name.toLatin1();
        for (const ASTDeclaration &param : func.params) {
            ret += param.name.toLatin1();
            ret += typeData(param.type, specialTypes);
            ret += QByteArray(reinterpret_cast<const char *>(&param.variableType), sizeof(param.variableType));
        }
        ret += typeData(func.returnType, specialTypes);
    }
    return ret;
}

QByteArray RepCodeGenerator::classSignature(const ASTClass &ac)
{
    QCryptographicHash checksum(QCryptographicHash::Sha1);
    QHash<QString, QByteArray> localTypes = m_globalEnumsPODs;
    for (const ASTEnum &e : ac.enums) // add local enums
        localTypes[e.name] = enumSignature(e);

    checksum.addData(ac.name.toLatin1());

    // Checksum properties
    for (const ASTProperty &p : ac.properties) {
        checksum.addData(p.name.toLatin1());
        checksum.addData(typeData(p.type, localTypes));
        checksum.addData(reinterpret_cast<const char *>(&p.modifier), sizeof(p.modifier));
    }

    // Checksum signals
    checksum.addData(functionsData(ac.signalsList, localTypes));

    // Checksum slots
    checksum.addData(functionsData(ac.slotsList, localTypes));

    return checksum.result().toHex();
}

void RepCodeGenerator::generate(const AST &ast, Mode mode, QString fileName)
{
    QTextStream stream(m_outputDevice);
    if (fileName.isEmpty())
        stream << "#pragma once" << Qt::endl << Qt::endl;
    else {
        fileName = QFileInfo(fileName).fileName();
        fileName = fileName.toUpper();
        fileName.replace(QLatin1Char('.'), QLatin1Char('_'));
        stream << "#ifndef " << fileName << Qt::endl;
        stream << "#define " << fileName << Qt::endl << Qt::endl;
    }

    generateHeader(mode, stream, ast);
    for (const ASTEnum &en : ast.enums)
        generateENUM(stream, en);
    for (const POD &pod : ast.pods)
        generatePOD(stream, pod);

    QSet<QString> metaTypes;
    for (const POD &pod : ast.pods) {
        metaTypes << pod.name;
        for (const PODAttribute &attribute : pod.attributes)
            metaTypes << attribute.type;
    }
    const QString metaTypeRegistrationCode = generateMetaTypeRegistration(metaTypes);

    for (const ASTClass &astClass : ast.classes) {
        QSet<QString> classMetaTypes;
        QSet<QString> pendingMetaTypes;
        for (const ASTProperty &property : astClass.properties) {
            if (property.isPointer)
                continue;
            classMetaTypes << property.type;
        }
        const auto extractClassMetaTypes = [&](const ASTFunction &function) {
            classMetaTypes << function.returnType;
            pendingMetaTypes << function.returnType;
            for (const ASTDeclaration &decl : function.params) {
                classMetaTypes << decl.type;
            }
        };
        for (const ASTFunction &function : astClass.signalsList)
            extractClassMetaTypes(function);
        for (const ASTFunction &function : astClass.slotsList)
            extractClassMetaTypes(function);

        const QString classMetaTypeRegistrationCode = metaTypeRegistrationCode
                + generateMetaTypeRegistration(classMetaTypes);
        const QString replicaMetaTypeRegistrationCode = classMetaTypeRegistrationCode
                + generateMetaTypeRegistrationForPending(pendingMetaTypes);

        if (mode == MERGED) {
            generateClass(REPLICA, stream, astClass, replicaMetaTypeRegistrationCode);
            generateClass(SOURCE, stream, astClass, classMetaTypeRegistrationCode);
            generateClass(SIMPLE_SOURCE, stream, astClass, classMetaTypeRegistrationCode);
            generateSourceAPI(stream, astClass);
        } else {
            generateClass(mode, stream, astClass, mode == REPLICA ? replicaMetaTypeRegistrationCode : classMetaTypeRegistrationCode);
            if (mode == SOURCE) {
                generateClass(SIMPLE_SOURCE, stream, astClass, classMetaTypeRegistrationCode);
                generateSourceAPI(stream, astClass);
            }
        }
    }

    generateStreamOperatorsForEnums(stream, ast.enumUses);

    stream << Qt::endl;
    if (!fileName.isEmpty())
        stream << "#endif // " << fileName << Qt::endl;
}

void RepCodeGenerator::generateHeader(Mode mode, QTextStream &out, const AST &ast)
{
    out << "// This is an autogenerated file.\n"
           "// Do not edit this file, any changes made will be lost the next time it is generated.\n"
           "\n"
           "#include <QtCore/qobject.h>\n"
           "#include <QtCore/qdatastream.h>\n"
           "#include <QtCore/qvariant.h>\n"
           "#include <QtCore/qmetatype.h>\n";
    bool hasModel = false;
    for (auto c : ast.classes)
    {
        if (c.modelMetadata.count() > 0)
        {
            hasModel = true;
            break;
        }
    }
    if (hasModel)
        out << "#include <QtCore/qabstractitemmodel.h>\n";
    out << "\n"
           "#include <QtRemoteObjects/qremoteobjectnode.h>\n";

    if (mode == MERGED) {
        out << "#include <QtRemoteObjects/qremoteobjectpendingcall.h>\n";
        out << "#include <QtRemoteObjects/qremoteobjectreplica.h>\n";
        out << "#include <QtRemoteObjects/qremoteobjectsource.h>\n";
        if (hasModel)
            out << "#include <QtRemoteObjects/qremoteobjectabstractitemmodelreplica.h>\n";
    } else if (mode == REPLICA) {
        out << "#include <QtRemoteObjects/qremoteobjectpendingcall.h>\n";
        out << "#include <QtRemoteObjects/qremoteobjectreplica.h>\n";
        if (hasModel)
            out << "#include <QtRemoteObjects/qremoteobjectabstractitemmodelreplica.h>\n";
    } else
        out << "#include <QtRemoteObjects/qremoteobjectsource.h>\n";
    out << "\n";

    out << ast.preprocessorDirectives.join(QLatin1Char('\n'));
    out << "\n";
}

static QString formatTemplateStringArgTypeNameCapitalizedName(int numberOfTypeOccurrences, int numberOfNameOccurrences,
                                                              QString templateString, const POD &pod)
{
    QString out;
    const int LengthOfPlaceholderText = 2;
    Q_ASSERT(templateString.count(QRegularExpression(QStringLiteral("%\\d"))) == numberOfNameOccurrences + numberOfTypeOccurrences);
    const auto expectedOutSize
            = numberOfNameOccurrences * accumulatedSizeOfNames(pod.attributes)
            + numberOfTypeOccurrences * accumulatedSizeOfTypes(pod.attributes)
            + pod.attributes.size() * (templateString.size() - (numberOfNameOccurrences + numberOfTypeOccurrences) * LengthOfPlaceholderText);
    out.reserve(expectedOutSize);
    for (const PODAttribute &a : pod.attributes)
        out += templateString.arg(a.type, a.name, cap(a.name));
    return out;
}

QString RepCodeGenerator::formatQPropertyDeclarations(const POD &pod)
{
    return formatTemplateStringArgTypeNameCapitalizedName(1, 3, QStringLiteral("    Q_PROPERTY(%1 %2 READ %2 WRITE set%3)\n"), pod);
}

QString RepCodeGenerator::formatConstructors(const POD &pod)
{
    QString initializerString = QStringLiteral(": ");
    QString defaultInitializerString = initializerString;
    QString argString;
    for (const PODAttribute &a : pod.attributes) {
        initializerString += QString::fromLatin1("m_%1(%1), ").arg(a.name);
        defaultInitializerString += QString::fromLatin1("m_%1(), ").arg(a.name);
        argString += QString::fromLatin1("%1 %2, ").arg(a.type, a.name);
    }
    argString.chop(2);
    initializerString.chop(2);
    defaultInitializerString.chop(2);

    return QString::fromLatin1("    %1() %2 {}\n"
                   "    explicit %1(%3) %4 {}\n")
            .arg(pod.name, defaultInitializerString, argString, initializerString);
}

QString RepCodeGenerator::formatPropertyGettersAndSetters(const POD &pod)
{
    // MSVC doesn't like adjacent string literal concatenation within QStringLiteral, so keep it in one line:
    QString templateString
            = QStringLiteral("    %1 %2() const { return m_%2; }\n    void set%3(%1 %2) { if (%2 != m_%2) { m_%2 = %2; } }\n");
    return formatTemplateStringArgTypeNameCapitalizedName(2, 8, qMove(templateString), pod);
}

QString RepCodeGenerator::formatDataMembers(const POD &pod)
{
    QString out;
    const QString prefix = QStringLiteral("    ");
    const QString infix  = QStringLiteral(" m_");
    const QString suffix = QStringLiteral(";\n");
    const auto expectedOutSize
            = accumulatedSizeOfNames(pod.attributes)
            + accumulatedSizeOfTypes(pod.attributes)
            + pod.attributes.size() * (prefix.size() + infix.size() + suffix.size());
    out.reserve(expectedOutSize);
    for (const PODAttribute &a : pod.attributes) {
        out += prefix;
        out += a.type;
        out += infix;
        out += a.name;
        out += suffix;
    }
    Q_ASSERT(out.size() == expectedOutSize);
    return out;
}

QString RepCodeGenerator::formatMarshallingOperators(const POD &pod)
{
    return QLatin1String("inline QDataStream &operator<<(QDataStream &ds, const ") + pod.name + QLatin1String(" &obj) {\n"
           "    QtRemoteObjects::copyStoredProperties(&obj, ds);\n"
           "    return ds;\n"
           "}\n"
           "\n"
           "inline QDataStream &operator>>(QDataStream &ds, ") + pod.name + QLatin1String(" &obj) {\n"
           "    QtRemoteObjects::copyStoredProperties(ds, &obj);\n"
           "    return ds;\n"
           "}\n")
           ;
}

QString RepCodeGenerator::typeForMode(const ASTProperty &property, RepCodeGenerator::Mode mode)
{
    if (!property.isPointer)
        return property.type;

    if (property.type.startsWith(QStringLiteral("QAbstractItemModel")))
        return mode == REPLICA ? property.type + QStringLiteral("Replica*") : property.type + QStringLiteral("*");

    switch (mode) {
    case REPLICA: return property.type + QStringLiteral("Replica*");
    case SIMPLE_SOURCE:
        Q_FALLTHROUGH();
    case SOURCE: return property.type + QStringLiteral("Source*");
    default: qCritical("Invalid mode");
    }

    return QStringLiteral("InvalidPropertyName");
}

void RepCodeGenerator::generateSimpleSetter(QTextStream &out, const ASTProperty &property, bool generateOverride)
{
    if (!generateOverride)
        out << "    virtual ";
    else
        out << "    ";
    out << "void set" << cap(property.name) << "(" << typeForMode(property, SIMPLE_SOURCE) << " " << property.name << ")";
    if (generateOverride)
        out << " override";
    out << Qt::endl;
    out << "    {" << Qt::endl;
    out << "        if (" << property.name << " != m_" << property.name << ") {" << Qt::endl;
    out << "            m_" << property.name << " = " << property.name << ";" << Qt::endl;
    out << "            Q_EMIT " << property.name << "Changed(m_" << property.name << ");" << Qt::endl;
    out << "        }" << Qt::endl;
    out << "    }" << Qt::endl;
}

void RepCodeGenerator::generatePOD(QTextStream &out, const POD &pod)
{
    QByteArray podData = pod.name.toLatin1();
    QStringList equalityCheck;
    for (const PODAttribute &attr : pod.attributes) {
        equalityCheck << QStringLiteral("left.%1() == right.%1()").arg(attr.name);
        podData += attr.name.toLatin1() + typeData(attr.type, m_globalEnumsPODs);
    }
    m_globalEnumsPODs[pod.name] = podData;
    out << "class " << pod.name << "\n"
           "{\n"
           "    Q_GADGET\n"
        << "\n"
        <<      formatQPropertyDeclarations(pod)
        << "public:\n"
        <<      formatConstructors(pod)
        <<      formatPropertyGettersAndSetters(pod)
        << "private:\n"
        <<      formatDataMembers(pod)
        << "};\n"
        << "\n"
        << "inline bool operator==(const " << pod.name << " &left, const " << pod.name << " &right) Q_DECL_NOTHROW {\n"
        << "    return " << equalityCheck.join(QStringLiteral(" && ")) << ";\n"
        << "}\n"
        << "inline bool operator!=(const " << pod.name << " &left, const " << pod.name << " &right) Q_DECL_NOTHROW {\n"
        << "    return !(left == right);\n"
        << "}\n"
        << "\n"
        << formatMarshallingOperators(pod)
        << "\n"
           "\n"
           ;
}

QString getEnumType(const ASTEnum &en)
{
    if (en.isSigned) {
        if (en.max < 0x7F)
            return QStringLiteral("qint8");
        if (en.max < 0x7FFF)
            return QStringLiteral("qint16");
        return QStringLiteral("qint32");
    } else {
        if (en.max < 0xFF)
            return QStringLiteral("quint8");
        if (en.max < 0xFFFF)
            return QStringLiteral("quint16");
        return QStringLiteral("quint32");
    }
}

void RepCodeGenerator::generateDeclarationsForEnums(QTextStream &out, const QList<ASTEnum> &enums, bool generateQENUM)
{
    if (!generateQENUM) {
        out << "    // You need to add this enum as well as Q_ENUM to your" << Qt::endl;
        out << "    // QObject class in order to use .rep enums over QtRO for" << Qt::endl;
        out << "    // non-repc generated QObjects." << Qt::endl;
    }
    for (const ASTEnum &en : enums) {
        m_globalEnumsPODs[en.name] = enumSignature(en);
        out << "    enum " << en.name << " {" << Qt::endl;
        for (const ASTEnumParam &p : en.params)
            out << "        " << p.name << " = " << p.value << "," << Qt::endl;

        out << "    };" << Qt::endl;

        if (generateQENUM)
            out << "    Q_ENUM(" << en.name << ")" << Qt::endl;
    }
}

void RepCodeGenerator::generateENUMs(QTextStream &out, const QList<ASTEnum> &enums, const QString &className)
{
    out << "class " << className << "\n"
           "{\n"
           "    Q_GADGET\n"
           "    " << className << "();\n"
           "\n"
           "public:\n";

    generateDeclarationsForEnums(out, enums);
    generateConversionFunctionsForEnums(out, enums);

    out << "};\n\n";

    generateStreamOperatorsForEnums(out, enums, className);
}

void RepCodeGenerator::generateConversionFunctionsForEnums(QTextStream &out, const QList<ASTEnum> &enums)
{
    for (const ASTEnum &en : enums)
    {
        const QString type = getEnumType(en);
        out << "    static inline " << en.name << " to" << en.name << "(" << type << " i, bool *ok = nullptr)\n"
               "    {\n"
               "        if (ok)\n"
               "            *ok = true;\n"
               "        switch (i) {\n";
            for (const ASTEnumParam &p : en.params)
                out << "        case " << p.value << ": return " << p.name << ";\n";
        out << "        default:\n"
               "            if (ok)\n"
               "                *ok = false;\n"
               "            return " << en.params.at(0).name << ";\n"
               "        }\n"
               "    }\n";
    }
}

void RepCodeGenerator::generateStreamOperatorsForEnums(QTextStream &out, const QList<ASTEnum> &enums, const QString &className)
{
    for (const ASTEnum &en : enums)
    {
        const QString type = getEnumType(en);
        out <<  "inline QDataStream &operator<<(QDataStream &ds, const " << className << "::" << en.name << " &obj)\n"
                "{\n"
                "    " << type << " val = obj;\n"
                "    ds << val;\n"
                "    return ds;\n"
                "}\n\n"

                "inline QDataStream &operator>>(QDataStream &ds, " << className << "::" << en.name << " &obj) {\n"
                "    bool ok;\n"
                "    " << type << " val;\n"
                "    ds >> val;\n"
                "    obj = " << className << "::to" << en.name << "(val, &ok);\n"
                "    if (!ok)\n        qWarning() << \"QtRO received an invalid enum value for type" << en.name << ", value =\" << val;\n"
                "    return ds;\n"
                "}\n\n";
    }
}

void RepCodeGenerator::generateENUM(QTextStream &out, const ASTEnum &en)
{
    generateENUMs(out, (QList<ASTEnum>() << en), QStringLiteral("%1Enum").arg(en.name));
}

QString RepCodeGenerator::generateMetaTypeRegistration(const QSet<QString> &metaTypes)
{
    QString out;
    const QString qRegisterMetaType = QStringLiteral("        qRegisterMetaType<");
    const QString lineEnding = QStringLiteral(">();\n");
    for (const QString &metaType : metaTypes) {
        if (isBuiltinType(metaType))
            continue;

        out += qRegisterMetaType;
        out += metaType;
        out += lineEnding;
    }
    return out;
}

QString RepCodeGenerator::generateMetaTypeRegistrationForPending(const QSet<QString> &metaTypes)
{
    QString out;
    if (!metaTypes.isEmpty())
        out += QLatin1String("        qRegisterMetaType<QRemoteObjectPendingCall>();\n");
    const QString qRegisterMetaType = QStringLiteral("        qRegisterMetaType<QRemoteObjectPendingReply<%1>>();\n");
    const QString qRegisterConverterConditional = QStringLiteral("        if (!QMetaType::hasRegisteredConverterFunction<QRemoteObjectPendingReply<%1>, QRemoteObjectPendingCall>())\n");
    const QString qRegisterConverter = QStringLiteral("            QMetaType::registerConverter<QRemoteObjectPendingReply<%1>, QRemoteObjectPendingCall>();\n");
    for (const QString &metaType : metaTypes) {
        out += qRegisterMetaType.arg(metaType);
        out += qRegisterConverterConditional.arg(metaType);
        out += qRegisterConverter.arg(metaType);
    }
    return out;
}

void RepCodeGenerator::generateStreamOperatorsForEnums(QTextStream &out, const QList<QString> &enumUses)
{
    out << "QT_BEGIN_NAMESPACE" << Qt::endl;
    for (const QString &enumName : enumUses) {
        out << "inline QDataStream &operator<<(QDataStream &out, " << enumName << " value)" << Qt::endl;
        out << "{" << Qt::endl;
        out << "    out << static_cast<qint32>(value);" << Qt::endl;
        out << "    return out;" << Qt::endl;
        out << "}" << Qt::endl;
        out << Qt::endl;
        out << "inline QDataStream &operator>>(QDataStream &in, " << enumName << " &value)" << Qt::endl;
        out << "{" << Qt::endl;
        out << "    qint32 intValue = 0;" << Qt::endl;
        out << "    in >> intValue;" << Qt::endl;
        out << "    value = static_cast<" << enumName << ">(intValue);" << Qt::endl;
        out << "    return in;" << Qt::endl;
        out << "}" << Qt::endl;
        out << Qt::endl;
    }
    out << "QT_END_NAMESPACE" << Qt::endl << Qt::endl;
}

void RepCodeGenerator::generateClass(Mode mode, QTextStream &out, const ASTClass &astClass, const QString &metaTypeRegistrationCode)
{
    const QString className = (astClass.name + (mode == REPLICA ? QStringLiteral("Replica") : mode == SOURCE ? QStringLiteral("Source") : QStringLiteral("SimpleSource")));
    if (mode == REPLICA)
        out << "class " << className << " : public QRemoteObjectReplica" << Qt::endl;
    else if (mode == SIMPLE_SOURCE)
        out << "class " << className << " : public " << astClass.name << "Source" << Qt::endl;
    else
        out << "class " << className << " : public QObject" << Qt::endl;

    out << "{" << Qt::endl;
    out << "    Q_OBJECT" << Qt::endl;
    if (mode != SIMPLE_SOURCE) {
        out << "    Q_CLASSINFO(QCLASSINFO_REMOTEOBJECT_TYPE, \"" << astClass.name << "\")" << Qt::endl;
        out << "    Q_CLASSINFO(QCLASSINFO_REMOTEOBJECT_SIGNATURE, \"" << QLatin1String(classSignature(astClass)) << "\")" << Qt::endl;
        for (int i = 0; i < astClass.modelMetadata.count(); i++) {
            const auto model = astClass.modelMetadata.at(i);
            const auto modelName = astClass.properties.at(model.propertyIndex).name;
            if (!model.roles.isEmpty()) {
                QStringList list;
                for (auto role : model.roles)
                    list << role.name;
                out << QString::fromLatin1("    Q_CLASSINFO(\"%1_ROLES\", \"%2\")").arg(modelName.toUpper(), list.join(QChar::fromLatin1('|'))) << Qt::endl;
            }
        }


        //First output properties
        for (const ASTProperty &property : astClass.properties) {
            out << "    Q_PROPERTY(" << typeForMode(property, mode) << " " << property.name << " READ " << property.name;
            if (property.modifier == ASTProperty::Constant) {
                if (mode == REPLICA) // We still need to notify when we get the initial value
                    out << " NOTIFY " << property.name << "Changed";
                else
                    out << " CONSTANT";
            } else if (property.modifier == ASTProperty::ReadOnly)
                out << " NOTIFY " << property.name << "Changed";
            else if (property.modifier == ASTProperty::ReadWrite)
                out << " WRITE set" << cap(property.name) << " NOTIFY " << property.name << "Changed";
            else if (property.modifier == ASTProperty::ReadPush || property.modifier == ASTProperty::SourceOnlySetter) {
                if (mode == REPLICA) // The setter slot isn't known to the PROP
                    out << " NOTIFY " << property.name << "Changed";
                else // The Source can use the setter, since non-asynchronous
                    out << " WRITE set" << cap(property.name) << " NOTIFY " << property.name << "Changed";
            }
            out << ")" << Qt::endl;
        }

        if (!astClass.enums.isEmpty()) {
            out << "" << Qt::endl;
            out << "public:" << Qt::endl;
            generateDeclarationsForEnums(out, astClass.enums);
        }
    }

    out << "" << Qt::endl;
    out << "public:" << Qt::endl;

    if (mode == REPLICA) {
        out << "    " << className << "() : QRemoteObjectReplica() { initialize(); }" << Qt::endl;
        out << "    static void registerMetatypes()" << Qt::endl;
        out << "    {" << Qt::endl;
        out << "        static bool initialized = false;" << Qt::endl;
        out << "        if (initialized)" << Qt::endl;
        out << "            return;" << Qt::endl;
        out << "        initialized = true;" << Qt::endl;

        if (!metaTypeRegistrationCode.isEmpty())
            out << metaTypeRegistrationCode << Qt::endl;

        out << "    }" << Qt::endl;

        if (astClass.hasPointerObjects())
        {
            out << "    void setNode(QRemoteObjectNode *node) override" << Qt::endl;
            out << "    {" << Qt::endl;
            out << "        QRemoteObjectReplica::setNode(node);" << Qt::endl;
            for (int index = 0; index < astClass.properties.count(); ++index) {
                const ASTProperty &property = astClass.properties.at(index);
                if (!property.isPointer)
                    continue;
                const QString acquireName = astClass.name + QLatin1String("::") + property.name;
                if (astClass.subClassPropertyIndices.contains(index))
                    out << QString::fromLatin1("        setChild(%1, QVariant::fromValue(node->acquire<%2Replica>(QRemoteObjectStringLiterals::CLASS().arg(\"%3\"))));")
                                              .arg(QString::number(index), property.type, acquireName) << Qt::endl;
                else
                    out << QString::fromLatin1("        setChild(%1, QVariant::fromValue(node->acquireModel(QRemoteObjectStringLiterals::MODEL().arg(\"%2\"))));")
                                              .arg(QString::number(index), acquireName) << Qt::endl;
                out << "        Q_EMIT " << property.name << "Changed(" << property.name << "()" << ");" << Qt::endl;

            }
            out << "    }" << Qt::endl;
        }
        out << "" << Qt::endl;
        out << "private:" << Qt::endl;
        out << "    " << className << "(QRemoteObjectNode *node, const QString &name = QString())" << Qt::endl;
        out << "        : QRemoteObjectReplica(ConstructWithNode)" << Qt::endl;
        out << "    {" << Qt::endl;
        out << "        initializeNode(node, name);" << Qt::endl;
        for (int index = 0; index < astClass.properties.count(); ++index) {
            const ASTProperty &property = astClass.properties.at(index);
            if (!property.isPointer)
                continue;
            const QString acquireName = astClass.name + QLatin1String("::") + property.name;
            if (astClass.subClassPropertyIndices.contains(index))
                out << QString::fromLatin1("        setChild(%1, QVariant::fromValue(node->acquire<%2Replica>(QRemoteObjectStringLiterals::CLASS().arg(\"%3\"))));")
                                          .arg(QString::number(index), property.type, acquireName) << Qt::endl;
            else
                out << QString::fromLatin1("        setChild(%1, QVariant::fromValue(node->acquireModel(QRemoteObjectStringLiterals::MODEL().arg(\"%2\"))));")
                                          .arg(QString::number(index), acquireName) << Qt::endl;
        }
        out << "    }" << Qt::endl;

        out << "" << Qt::endl;

        out << "    void initialize() override" << Qt::endl;
        out << "    {" << Qt::endl;
        out << "        " << className << "::registerMetatypes();" << Qt::endl;
        out << "        QVariantList properties;" << Qt::endl;
        out << "        properties.reserve(" << astClass.properties.size() << ");" << Qt::endl;
        for (const ASTProperty &property : astClass.properties) {
            if (property.isPointer)
                out << "        properties << QVariant::fromValue((" << typeForMode(property, mode) << ")" << property.defaultValue << ");" << Qt::endl;
            else
                out << "        properties << QVariant::fromValue(" << typeForMode(property, mode) << "(" << property.defaultValue << "));" << Qt::endl;
        }
        int nPersisted = 0;
        if (astClass.hasPersisted) {
            out << "        QVariantList stored = retrieveProperties(\"" << astClass.name << "\", \"" << classSignature(astClass) << "\");" << Qt::endl;
            out << "        if (!stored.isEmpty()) {" << Qt::endl;
            for (int i = 0; i < astClass.properties.size(); i++) {
                if (astClass.properties.at(i).persisted) {
                    out << "            properties[" << i << "] = stored.at(" << nPersisted << ");" << Qt::endl;
                    nPersisted++;
                }
            }
            out << "        }" << Qt::endl;
        }
        out << "        setProperties(properties);" << Qt::endl;
        out << "    }" << Qt::endl;
    } else if (mode == SOURCE) {
        out << "    explicit " << className << "(QObject *parent = nullptr) : QObject(parent)" << Qt::endl;
        out << "    {" << Qt::endl;
        if (!metaTypeRegistrationCode.isEmpty())
            out << metaTypeRegistrationCode << Qt::endl;
        out << "    }" << Qt::endl;
    } else {
        QList<int> constIndices;
        for (int index = 0; index < astClass.properties.count(); ++index) {
            const ASTProperty &property = astClass.properties.at(index);
            if (property.modifier == ASTProperty::Constant)
                constIndices.append(index);
        }
        if (constIndices.isEmpty()) {
            out << "    explicit " << className << "(QObject *parent = nullptr) : " << astClass.name << "Source(parent)" << Qt::endl;
        } else {
            QStringList parameters;
            for (int index : constIndices) {
                const ASTProperty &property = astClass.properties.at(index);
                parameters.append(QString::fromLatin1("%1 %2 = %3").arg(typeForMode(property, SOURCE), property.name, property.defaultValue));
            }
            parameters.append(QStringLiteral("QObject *parent = nullptr"));
            out << "    explicit " << className << "(" << parameters.join(QStringLiteral(", ")) << ") : " << astClass.name << "Source(parent)" << Qt::endl;
        }
        for (const ASTProperty &property : astClass.properties) {
            if (property.modifier == ASTProperty::Constant)
                out << "    , m_" << property.name << "(" << property.name << ")" << Qt::endl;
            else
                out << "    , m_" << property.name << "(" << property.defaultValue << ")" << Qt::endl;
        }
        out << "    {" << Qt::endl;
        out << "    }" << Qt::endl;
    }

    out << "" << Qt::endl;
    out << "public:" << Qt::endl;

    if (mode == REPLICA && astClass.hasPersisted) {
        out << "    ~" << className << "() override {" << Qt::endl;
        out << "        QVariantList persisted;" << Qt::endl;
        for (int i = 0; i < astClass.properties.size(); i++) {
            if (astClass.properties.at(i).persisted) {
                out << "        persisted << propAsVariant(" << i << ");" << Qt::endl;
            }
        }
        out << "        persistProperties(\"" << astClass.name << "\", \"" << classSignature(astClass) << "\", persisted);" << Qt::endl;
        out << "    }" << Qt::endl;
    } else {
        out << "    ~" << className << "() override = default;" << Qt::endl;
    }
    out << "" << Qt::endl;

    if (mode != SIMPLE_SOURCE)
        generateConversionFunctionsForEnums(out, astClass.enums);

    //Next output getter/setter
    if (mode == REPLICA) {
        int i = 0;
        for (const ASTProperty &property : astClass.properties) {
            auto type = typeForMode(property, mode);
            if (type == QLatin1String("QVariant")) {
                out << "    " << type << " " << property.name << "() const" << Qt::endl;
                out << "    {" << Qt::endl;
                out << "        return propAsVariant(" << i << ");" << Qt::endl;
                out << "    }" << Qt::endl;
            } else {
                out << "    " << type << " " << property.name << "() const" << Qt::endl;
                out << "    {" << Qt::endl;
                out << "        const QVariant variant = propAsVariant(" << i << ");" << Qt::endl;
                out << "        if (!variant.canConvert<" << type << ">()) {" << Qt::endl;
                out << "            qWarning() << \"QtRO cannot convert the property " << property.name << " to type " << type << "\";" << Qt::endl;
                out << "        }" << Qt::endl;
                out << "        return variant.value<" << type << " >();" << Qt::endl;
                out << "    }" << Qt::endl;
            }
            i++;
            if (property.modifier == ASTProperty::ReadWrite) {
                out << "" << Qt::endl;
                out << "    void set" << cap(property.name) << "(" << property.type << " " << property.name << ")" << Qt::endl;
                out << "    {" << Qt::endl;
                out << "        static int __repc_index = " << className << "::staticMetaObject.indexOfProperty(\"" << property.name << "\");" << Qt::endl;
                out << "        QVariantList __repc_args;" << Qt::endl;
                out << "        __repc_args << QVariant::fromValue(" << property.name << ");" << Qt::endl;
                out << "        send(QMetaObject::WriteProperty, __repc_index, __repc_args);" << Qt::endl;
                out << "    }" << Qt::endl;
            }
            out << "" << Qt::endl;
        }
    } else if (mode == SOURCE) {
        for (const ASTProperty &property : astClass.properties)
            out << "    virtual " << typeForMode(property, mode) << " " << property.name << "() const = 0;" << Qt::endl;
        for (const ASTProperty &property : astClass.properties) {
            if (property.modifier == ASTProperty::ReadWrite ||
                    property.modifier == ASTProperty::ReadPush ||
                    property.modifier == ASTProperty::SourceOnlySetter)
                out << "    virtual void set" << cap(property.name) << "(" << typeForMode(property, mode) << " " << property.name << ") = 0;" << Qt::endl;
        }
    } else {
        for (const ASTProperty &property : astClass.properties)
            out << "    " << typeForMode(property, mode) << " " << property.name << "() const override { return m_"
                << property.name << "; }" << Qt::endl;
        for (const ASTProperty &property : astClass.properties) {
            if (property.modifier == ASTProperty::ReadWrite ||
                    property.modifier == ASTProperty::ReadPush ||
                    property.modifier == ASTProperty::SourceOnlySetter) {
                generateSimpleSetter(out, property);
            }
        }
    }

    if (mode != SIMPLE_SOURCE) {
        //Next output property signals
        if (!astClass.properties.isEmpty() || !astClass.signalsList.isEmpty()) {
            out << "" << Qt::endl;
            out << "Q_SIGNALS:" << Qt::endl;
            for (const ASTProperty &property : astClass.properties) {
                if (property.modifier != ASTProperty::Constant)
                    out << "    void " << property.name << "Changed(" << fullyQualifiedTypeName(astClass, className, typeForMode(property, mode)) << " " << property.name << ");" << Qt::endl;
            }

            const QList<ASTFunction> signalsList = transformEnumParams(astClass, astClass.signalsList, className);
            for (const ASTFunction &signal : signalsList)
                out << "    void " << signal.name << "(" << signal.paramsAsString() << ");" << Qt::endl;

            // CONSTANT source properties still need an onChanged signal on the Replica side to
            // update (once) when the value is initialized.  Put these last, so they don't mess
            // up the signal index order
            for (const ASTProperty &property : astClass.properties) {
                if (mode == REPLICA && property.modifier == ASTProperty::Constant)
                    out << "    void " << property.name << "Changed(" << fullyQualifiedTypeName(astClass, className, typeForMode(property, mode)) << " " << property.name << ");" << Qt::endl;
            }
        }
        bool hasWriteSlots = false;
        for (const ASTProperty &property : astClass.properties) {
            if (property.modifier == ASTProperty::ReadPush) {
                hasWriteSlots = true;
                break;
            }
        }
        if (hasWriteSlots || !astClass.slotsList.isEmpty()) {
            out << "" << Qt::endl;
            out << "public Q_SLOTS:" << Qt::endl;
            for (const ASTProperty &property : astClass.properties) {
                if (property.modifier == ASTProperty::ReadPush) {
                    const auto type = fullyQualifiedTypeName(astClass, className, property.type);
                    if (mode != REPLICA) {
                        out << "    virtual void push" << cap(property.name) << "(" << type << " " << property.name << ")" << Qt::endl;
                        out << "    {" << Qt::endl;
                        out << "        set" << cap(property.name) << "(" << property.name << ");" << Qt::endl;
                        out << "    }" << Qt::endl;
                    } else {
                        out << "    void push" << cap(property.name) << "(" << type << " " << property.name << ")" << Qt::endl;
                        out << "    {" << Qt::endl;
                        out << "        static int __repc_index = " << className << "::staticMetaObject.indexOfSlot(\"push" << cap(property.name) << "(" << type << ")\");" << Qt::endl;
                        out << "        QVariantList __repc_args;" << Qt::endl;
                        out << "        __repc_args << QVariant::fromValue(" << property.name << ");" << Qt::endl;
                        out << "        send(QMetaObject::InvokeMetaMethod, __repc_index, __repc_args);" << Qt::endl;
                        out << "    }" << Qt::endl;
                    }
                }
            }
            const QList<ASTFunction> slotsList = transformEnumParams(astClass, astClass.slotsList, className);
            for (const ASTFunction &slot : slotsList) {
                const auto returnType = fullyQualifiedTypeName(astClass, className, slot.returnType);
                if (mode != REPLICA) {
                    out << "    virtual " << returnType << " " << slot.name << "(" << slot.paramsAsString() << ") = 0;" << Qt::endl;
                } else {
                    // TODO: Discuss whether it is a good idea to special-case for void here,
                    const bool isVoid = slot.returnType == QStringLiteral("void");

                    if (isVoid)
                        out << "    void " << slot.name << "(" << slot.paramsAsString() << ")" << Qt::endl;
                    else
                        out << "    QRemoteObjectPendingReply<" << returnType << "> " << slot.name << "(" << slot.paramsAsString()<< ")" << Qt::endl;
                    out << "    {" << Qt::endl;
                    out << "        static int __repc_index = " << className << "::staticMetaObject.indexOfSlot(\"" << slot.name << "(" << slot.paramsAsString(ASTFunction::Normalized) << ")\");" << Qt::endl;
                    out << "        QVariantList __repc_args;" << Qt::endl;
                    const auto &paramNames = slot.paramNames();
                    if (!paramNames.isEmpty()) {
                        out << "        __repc_args" << Qt::endl;
                        for (const QString &name : paramNames)
                            out << "            << " << "QVariant::fromValue(" << name << ")" << Qt::endl;
                        out << "        ;" << Qt::endl;
                    }
                    if (isVoid)
                        out << "        send(QMetaObject::InvokeMetaMethod, __repc_index, __repc_args);" << Qt::endl;
                    else
                        out << "        return QRemoteObjectPendingReply<" << returnType << ">(sendWithReply(QMetaObject::InvokeMetaMethod, __repc_index, __repc_args));" << Qt::endl;
                    out << "    }" << Qt::endl;
                }
            }
        }
    } else {
        if (!astClass.properties.isEmpty()) {
            bool addProtected = true;
            for (const ASTProperty &property : astClass.properties) {
                if (property.modifier == ASTProperty::ReadOnly) {
                    if (addProtected) {
                        out << "" << Qt::endl;
                        out << "protected:" << Qt::endl;
                        addProtected = false;
                    }
                    generateSimpleSetter(out, property, false);
                }
            }
        }
    }

    out << "" << Qt::endl;
    out << "private:" << Qt::endl;

    //Next output data members
    if (mode == SIMPLE_SOURCE) {
        for (const ASTProperty &property : astClass.properties)
            out << "    " << typeForMode(property, SOURCE) << " " << "m_" << property.name << ";" << Qt::endl;
    }

    if (mode != SIMPLE_SOURCE)
        out << "    friend class QT_PREPEND_NAMESPACE(QRemoteObjectNode);" << Qt::endl;

    out << "};" << Qt::endl;
    out << "" << Qt::endl;

    if (mode != SIMPLE_SOURCE)
        generateStreamOperatorsForEnums(out, astClass.enums, className);

    out << "" << Qt::endl;
}

void RepCodeGenerator::generateSourceAPI(QTextStream &out, const ASTClass &astClass)
{
    const QString className = astClass.name + QStringLiteral("SourceAPI");
    out << QStringLiteral("template <class ObjectType>") << Qt::endl;
    out << QString::fromLatin1("struct %1 : public SourceApiMap").arg(className) << Qt::endl;
    out << QStringLiteral("{") << Qt::endl;
    if (!astClass.enums.isEmpty()) {
        // Include enum definition in SourceAPI
        generateDeclarationsForEnums(out, astClass.enums, false);
    }
    out << QString::fromLatin1("    %1(ObjectType *object, const QString &name = QLatin1String(\"%2\"))").arg(className, astClass.name) << Qt::endl;
    out << QStringLiteral("        : SourceApiMap(), m_name(name)") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    if (!astClass.hasPointerObjects())
        out << QStringLiteral("        Q_UNUSED(object)") << Qt::endl;

    const auto enumCount = astClass.enums.count();
    for (int i : astClass.subClassPropertyIndices) {
        const ASTProperty &child = astClass.properties.at(i);
        out << QString::fromLatin1("        using %1_type_t = typename std::remove_pointer<decltype(object->%1())>::type;")
                                  .arg(child.name) << Qt::endl;
    }
    out << QString::fromLatin1("        m_enums[0] = %1;").arg(enumCount) << Qt::endl;
    for (qsizetype i = 0; i < enumCount; ++i) {
        const auto enumerator = astClass.enums.at(i);
        out << QString::fromLatin1("        m_enums[%1] = ObjectType::staticMetaObject.indexOfEnumerator(\"%2\");")
                             .arg(i+1).arg(enumerator.name) << Qt::endl;
    }
    const auto propCount = astClass.properties.count();
    out << QString::fromLatin1("        m_properties[0] = %1;").arg(propCount) << Qt::endl;
    QList<ASTProperty> onChangeProperties;
    QList<qsizetype> propertyChangeIndex;
    for (qsizetype i = 0; i < propCount; ++i) {
        const ASTProperty &prop = astClass.properties.at(i);
        const QString propTypeName = fullyQualifiedTypeName(astClass, QStringLiteral("typename ObjectType"), typeForMode(prop, SOURCE));
        out << QString::fromLatin1("        m_properties[%1] = QtPrivate::qtro_property_index<ObjectType>(&ObjectType::%2, "
                              "static_cast<%3 (QObject::*)()>(nullptr),\"%2\");")
                             .arg(QString::number(i+1), prop.name, propTypeName) << Qt::endl;
        if (prop.modifier == prop.ReadWrite) //Make sure we have a setter function
            out << QStringLiteral("        QtPrivate::qtro_method_test<ObjectType>(&ObjectType::set%1, static_cast<void (QObject::*)(%2)>(nullptr));")
                                 .arg(cap(prop.name), propTypeName) << Qt::endl;
        if (prop.modifier != prop.Constant) { //Make sure we have an onChange signal
            out << QStringLiteral("        QtPrivate::qtro_method_test<ObjectType>(&ObjectType::%1Changed, static_cast<void (QObject::*)()>(nullptr));")
                                 .arg(prop.name) << Qt::endl;
            onChangeProperties << prop;
            propertyChangeIndex << i + 1; //m_properties[0] is the count, so index is one higher
        }
    }
    const auto signalCount = astClass.signalsList.count();
    const auto changedCount = onChangeProperties.size();
    out << QString::fromLatin1("        m_signals[0] = %1;").arg(signalCount+onChangeProperties.size()) << Qt::endl;
    for (qsizetype i = 0; i < changedCount; ++i)
        out << QString::fromLatin1("        m_signals[%1] = QtPrivate::qtro_signal_index<ObjectType>(&ObjectType::%2Changed, "
                              "static_cast<void (QObject::*)(%3)>(nullptr),m_signalArgCount+%4,&m_signalArgTypes[%4]);")
                             .arg(QString::number(i+1), onChangeProperties.at(i).name,
                                  fullyQualifiedTypeName(astClass, QStringLiteral("typename ObjectType"), typeForMode(onChangeProperties.at(i), SOURCE)),
                                  QString::number(i)) << Qt::endl;

    QList<ASTFunction> signalsList = transformEnumParams(astClass, astClass.signalsList, QStringLiteral("typename ObjectType"));
    for (qsizetype i = 0; i < signalCount; ++i) {
        const ASTFunction &sig = signalsList.at(i);
        out << QString::fromLatin1("        m_signals[%1] = QtPrivate::qtro_signal_index<ObjectType>(&ObjectType::%2, "
                              "static_cast<void (QObject::*)(%3)>(nullptr),m_signalArgCount+%4,&m_signalArgTypes[%4]);")
                             .arg(QString::number(changedCount+i+1), sig.name, sig.paramsAsString(ASTFunction::Normalized), QString::number(changedCount+i)) << Qt::endl;
    }
    const auto slotCount = astClass.slotsList.count();
    QList<ASTProperty> pushProps;
    for (const ASTProperty &property : astClass.properties) {
        if (property.modifier == ASTProperty::ReadPush)
            pushProps << property;
    }
    const auto pushCount = pushProps.count();
    const auto methodCount = slotCount + pushCount;
    out << QString::fromLatin1("        m_methods[0] = %1;").arg(methodCount) << Qt::endl;
    for (qsizetype i = 0; i < pushCount; ++i) {
        const ASTProperty &prop = pushProps.at(i);
        const QString propTypeName = fullyQualifiedTypeName(astClass, QStringLiteral("typename ObjectType"), prop.type);
        out << QString::fromLatin1("        m_methods[%1] = QtPrivate::qtro_method_index<ObjectType>(&ObjectType::push%2, "
                              "static_cast<void (QObject::*)(%3)>(nullptr),\"push%2(%4)\",m_methodArgCount+%5,&m_methodArgTypes[%5]);")
                             .arg(QString::number(i+1), cap(prop.name), propTypeName,
                                  QString(propTypeName).remove(QStringLiteral("typename ObjectType::")), // we don't want this in the string signature
                                  QString::number(i)) << Qt::endl;
    }

    QList<ASTFunction> slotsList = transformEnumParams(astClass, astClass.slotsList, QStringLiteral("typename ObjectType"));
    for (qsizetype i = 0; i < slotCount; ++i) {
        const ASTFunction &slot = slotsList.at(i);
        const QString params = slot.paramsAsString(ASTFunction::Normalized);
        out << QString::fromLatin1("        m_methods[%1] = QtPrivate::qtro_method_index<ObjectType>(&ObjectType::%2, "
                              "static_cast<void (QObject::*)(%3)>(nullptr),\"%2(%4)\",m_methodArgCount+%5,&m_methodArgTypes[%5]);")
                             .arg(QString::number(i+pushCount+1), slot.name, params,
                                  QString(params).remove(QStringLiteral("typename ObjectType::")), // we don't want this in the string signature
                                  QString::number(i+pushCount)) << Qt::endl;
    }
    for (const auto &model : astClass.modelMetadata) {
        const ASTProperty &property = astClass.properties.at(model.propertyIndex);
        out << QString::fromLatin1("        m_models << ModelInfo({object->%1(),").arg(property.name) << Qt::endl;
        out << QString::fromLatin1("                               QStringLiteral(\"%1\"),").arg(property.name) << Qt::endl;
        QStringList list;
        if (!model.roles.isEmpty()) {
            for (auto role : model.roles)
                list << role.name;
        }
        out << QString::fromLatin1("                               QByteArrayLiteral(\"%1\")});").arg(list.join(QChar::fromLatin1('|'))) << Qt::endl;
    }
    for (int i : astClass.subClassPropertyIndices) {
        const ASTProperty &child = astClass.properties.at(i);
        out << QString::fromLatin1("        m_subclasses << new %2SourceAPI<%1_type_t>(object->%1(), QStringLiteral(\"%1\"));")
                                   .arg(child.name, child.type) << Qt::endl;
    }
    out << QStringLiteral("    }") << Qt::endl;
    out << QStringLiteral("") << Qt::endl;
    out << QString::fromLatin1("    QString name() const override { return m_name; }") << Qt::endl;
    out << QString::fromLatin1("    QString typeName() const override { return QStringLiteral(\"%1\"); }").arg(astClass.name) << Qt::endl;
    out << QStringLiteral("    int enumCount() const override { return m_enums[0]; }") << Qt::endl;
    out << QStringLiteral("    int propertyCount() const override { return m_properties[0]; }") << Qt::endl;
    out << QStringLiteral("    int signalCount() const override { return m_signals[0]; }") << Qt::endl;
    out << QStringLiteral("    int methodCount() const override { return m_methods[0]; }") << Qt::endl;
    out << QStringLiteral("    int sourceEnumIndex(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    out << QStringLiteral("        if (index < 0 || index >= m_enums[0])") << Qt::endl;
    out << QStringLiteral("            return -1;") << Qt::endl;
    out << QStringLiteral("        return m_enums[index+1];") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;
    out << QStringLiteral("    int sourcePropertyIndex(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    out << QStringLiteral("        if (index < 0 || index >= m_properties[0])") << Qt::endl;
    out << QStringLiteral("            return -1;") << Qt::endl;
    out << QStringLiteral("        return m_properties[index+1];") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;
    out << QStringLiteral("    int sourceSignalIndex(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    out << QStringLiteral("        if (index < 0 || index >= m_signals[0])") << Qt::endl;
    out << QStringLiteral("            return -1;") << Qt::endl;
    out << QStringLiteral("        return m_signals[index+1];") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;
    out << QStringLiteral("    int sourceMethodIndex(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    out << QStringLiteral("        if (index < 0 || index >= m_methods[0])") << Qt::endl;
    out << QStringLiteral("            return -1;") << Qt::endl;
    out << QStringLiteral("        return m_methods[index+1];") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;
    if (signalCount+changedCount > 0) {
        out << QStringLiteral("    int signalParameterCount(int index) const override") << Qt::endl;
        out << QStringLiteral("    {") << Qt::endl;
        out << QStringLiteral("        if (index < 0 || index >= m_signals[0])") << Qt::endl;
        out << QStringLiteral("            return -1;") << Qt::endl;
        out << QStringLiteral("        return m_signalArgCount[index];") << Qt::endl;
        out << QStringLiteral("    }") << Qt::endl;
        out << QStringLiteral("    int signalParameterType(int sigIndex, int paramIndex) const override") << Qt::endl;
        out << QStringLiteral("    {") << Qt::endl;
        out << QStringLiteral("        if (sigIndex < 0 || sigIndex >= m_signals[0] || paramIndex < 0 || paramIndex >= m_signalArgCount[sigIndex])") << Qt::endl;
        out << QStringLiteral("            return -1;") << Qt::endl;
        out << QStringLiteral("        return m_signalArgTypes[sigIndex][paramIndex];") << Qt::endl;
        out << QStringLiteral("    }") << Qt::endl;
    } else {
        out << QStringLiteral("    int signalParameterCount(int index) const override { Q_UNUSED(index) return -1; }") << Qt::endl;
        out << QStringLiteral("    int signalParameterType(int sigIndex, int paramIndex) const override") << Qt::endl;
        out << QStringLiteral("    { Q_UNUSED(sigIndex) Q_UNUSED(paramIndex) return -1; }") << Qt::endl;
    }
    if (methodCount > 0) {
        out << QStringLiteral("    int methodParameterCount(int index) const override") << Qt::endl;
        out << QStringLiteral("    {") << Qt::endl;
        out << QStringLiteral("        if (index < 0 || index >= m_methods[0])") << Qt::endl;
        out << QStringLiteral("            return -1;") << Qt::endl;
        out << QStringLiteral("        return m_methodArgCount[index];") << Qt::endl;
        out << QStringLiteral("    }") << Qt::endl;
        out << QStringLiteral("    int methodParameterType(int methodIndex, int paramIndex) const override") << Qt::endl;
        out << QStringLiteral("    {") << Qt::endl;
        out << QStringLiteral("        if (methodIndex < 0 || methodIndex >= m_methods[0] || paramIndex < 0 || paramIndex >= m_methodArgCount[methodIndex])") << Qt::endl;
        out << QStringLiteral("            return -1;") << Qt::endl;
        out << QStringLiteral("        return m_methodArgTypes[methodIndex][paramIndex];") << Qt::endl;
        out << QStringLiteral("    }") << Qt::endl;
    } else {
        out << QStringLiteral("    int methodParameterCount(int index) const override { Q_UNUSED(index) return -1; }") << Qt::endl;
        out << QStringLiteral("    int methodParameterType(int methodIndex, int paramIndex) const override") << Qt::endl;
        out << QStringLiteral("    { Q_UNUSED(methodIndex) Q_UNUSED(paramIndex) return -1; }") << Qt::endl;
    }
    //propertyIndexFromSignal method
    out << QStringLiteral("    int propertyIndexFromSignal(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    if (!propertyChangeIndex.isEmpty()) {
        out << QStringLiteral("        switch (index) {") << Qt::endl;
        for (int i = 0; i < propertyChangeIndex.size(); ++i)
            out << QString::fromLatin1("        case %1: return m_properties[%2];").arg(i).arg(propertyChangeIndex.at(i)) << Qt::endl;
        out << QStringLiteral("        }") << Qt::endl;
    } else
        out << QStringLiteral("        Q_UNUSED(index)") << Qt::endl;
    out << QStringLiteral("        return -1;") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;
    //propertyRawIndexFromSignal method
    out << QStringLiteral("    int propertyRawIndexFromSignal(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    if (!propertyChangeIndex.isEmpty()) {
        out << QStringLiteral("        switch (index) {") << Qt::endl;
        for (int i = 0; i < propertyChangeIndex.size(); ++i)
            out << QString::fromLatin1("        case %1: return %2;").arg(i).arg(propertyChangeIndex.at(i)-1) << Qt::endl;
        out << QStringLiteral("        }") << Qt::endl;
    } else
        out << QStringLiteral("        Q_UNUSED(index)") << Qt::endl;
    out << QStringLiteral("        return -1;") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;

    //signalSignature method
    out << QStringLiteral("    const QByteArray signalSignature(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    if (signalCount+changedCount > 0) {
        out << QStringLiteral("        switch (index) {") << Qt::endl;
        for (int i = 0; i < changedCount; ++i) {
            const ASTProperty &prop = onChangeProperties.at(i);
            if (isClassEnum(astClass, prop.type))
                out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"%2Changed($1)\").replace(\"$1\", QtPrivate::qtro_enum_signature<ObjectType>(\"%3\"));")
                    .arg(QString::number(i), prop.name, prop.type) << Qt::endl;
            else
                out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"%2Changed(%3)\");")
                    .arg(QString::number(i), prop.name, typeForMode(prop, SOURCE)) << Qt::endl;
        }
        for (int i = 0; i < signalCount; ++i)
        {
            const ASTFunction &sig = astClass.signalsList.at(i);
            auto paramsAsString = sig.paramsAsString(ASTFunction::Normalized);
            const auto paramsAsList = paramsAsString.split(QLatin1String(","));
            int enumCount = 0;
            QString enumString;
            for (int j = 0; j < paramsAsList.count(); j++) {
                auto const p = paramsAsList.at(j);
                if (isClassEnum(astClass, p)) {
                    paramsAsString.replace(paramsAsString.indexOf(p), p.size(), QStringLiteral("$%1").arg(enumCount));
                    enumString.append(QString::fromLatin1(".replace(\"$%1\", QtPrivate::qtro_enum_signature<ObjectType>(\"%2\"))").arg(enumCount++).arg(paramsAsList.at(j)));
                }
            }
            out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"%2(%3)\")%4;")
                                    .arg(QString::number(i+changedCount), sig.name, paramsAsString, enumString) << Qt::endl;
        }
        out << QStringLiteral("        }") << Qt::endl;
    } else
        out << QStringLiteral("        Q_UNUSED(index)") << Qt::endl;
    out << QStringLiteral("        return QByteArrayLiteral(\"\");") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;

    //signalParameterNames method
    out << QStringLiteral("    QByteArrayList signalParameterNames(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    out << QStringLiteral("        if (index < 0 || index >= m_signals[0])") << Qt::endl;
    out << QStringLiteral("            return QByteArrayList();") << Qt::endl;
    out << QStringLiteral("        return ObjectType::staticMetaObject.method(m_signals[index + 1]).parameterNames();") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;

    //methodSignature method
    out << QStringLiteral("    const QByteArray methodSignature(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    if (methodCount > 0) {
        out << QStringLiteral("        switch (index) {") << Qt::endl;
        for (int i = 0; i < pushCount; ++i)
        {
            const ASTProperty &prop = pushProps.at(i);
            if (isClassEnum(astClass, prop.type))
                out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"push%2($1)\").replace(\"$1\", QtPrivate::qtro_enum_signature<ObjectType>(\"%3\"));")
                    .arg(QString::number(i), prop.name, prop.type) << Qt::endl;
            else
                out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"push%2(%3)\");")
                                        .arg(QString::number(i), cap(prop.name), prop.type) << Qt::endl;
        }
        for (int i = 0; i < slotCount; ++i)
        {
            const ASTFunction &slot = astClass.slotsList.at(i);
            auto paramsAsString = slot.paramsAsString(ASTFunction::Normalized);
            const auto paramsAsList = paramsAsString.split(QLatin1String(","));
            int enumCount = 0;
            QString enumString;
            for (int j = 0; j < paramsAsList.count(); j++) {
                auto const p = paramsAsList.at(j);
                if (isClassEnum(astClass, p)) {
                    paramsAsString.replace(paramsAsString.indexOf(p), p.size(), QStringLiteral("$%1").arg(enumCount));
                    enumString.append(QString::fromLatin1(".replace(\"$%1\", QtPrivate::qtro_enum_signature<ObjectType>(\"%2\"))").arg(enumCount++).arg(paramsAsList.at(j)));
                }
            }
            out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"%2(%3)\")%4;")
                                    .arg(QString::number(i+pushCount), slot.name, paramsAsString, enumString) << Qt::endl;
        }
        out << QStringLiteral("        }") << Qt::endl;
    } else
        out << QStringLiteral("        Q_UNUSED(index)") << Qt::endl;
    out << QStringLiteral("        return QByteArrayLiteral(\"\");") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;

    //methodType method
    out << QStringLiteral("    QMetaMethod::MethodType methodType(int) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    out << QStringLiteral("        return QMetaMethod::Slot;") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;

    //methodParameterNames method
    out << QStringLiteral("    QByteArrayList methodParameterNames(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    out << QStringLiteral("        if (index < 0 || index >= m_methods[0])") << Qt::endl;
    out << QStringLiteral("            return QByteArrayList();") << Qt::endl;
    out << QStringLiteral("        return ObjectType::staticMetaObject.method(m_methods[index + 1]).parameterNames();") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;

    //typeName method
    out << QStringLiteral("    const QByteArray typeName(int index) const override") << Qt::endl;
    out << QStringLiteral("    {") << Qt::endl;
    if (methodCount > 0) {
        out << QStringLiteral("        switch (index) {") << Qt::endl;
        for (int i = 0; i < pushCount; ++i)
        {
            out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"void\");")
                                      .arg(QString::number(i)) << Qt::endl;
        }
        for (int i = 0; i < slotCount; ++i)
        {
            const ASTFunction &slot = astClass.slotsList.at(i);
            if (isClassEnum(astClass, slot.returnType))
                out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"$1\").replace(\"$1\", QtPrivate::qtro_enum_signature<ObjectType>(\"%2\"));")
                                          .arg(QString::number(i+pushCount), slot.returnType) << Qt::endl;
            else
                out << QString::fromLatin1("        case %1: return QByteArrayLiteral(\"%2\");")
                                          .arg(QString::number(i+pushCount), slot.returnType) << Qt::endl;
        }
        out << QStringLiteral("        }") << Qt::endl;
    } else
        out << QStringLiteral("        Q_UNUSED(index)") << Qt::endl;
    out << QStringLiteral("        return QByteArrayLiteral(\"\");") << Qt::endl;
    out << QStringLiteral("    }") << Qt::endl;

    //objectSignature method
    out << QStringLiteral("    QByteArray objectSignature() const override { return QByteArray{\"")
        << QLatin1String(classSignature(astClass))
        << QStringLiteral("\"}; }") << Qt::endl;

    out << QStringLiteral("") << Qt::endl;
    out << QString::fromLatin1("    int m_enums[%1];").arg(enumCount + 1) << Qt::endl;
    out << QString::fromLatin1("    int m_properties[%1];").arg(propCount+1) << Qt::endl;
    out << QString::fromLatin1("    int m_signals[%1];").arg(signalCount+changedCount+1) << Qt::endl;
    out << QString::fromLatin1("    int m_methods[%1];").arg(methodCount+1) << Qt::endl;
    out << QString::fromLatin1("    const QString m_name;") << Qt::endl;
    if (signalCount+changedCount > 0) {
        out << QString::fromLatin1("    int m_signalArgCount[%1];").arg(signalCount+changedCount) << Qt::endl;
        out << QString::fromLatin1("    const int* m_signalArgTypes[%1];").arg(signalCount+changedCount) << Qt::endl;
    }
    if (methodCount > 0) {
        out << QString::fromLatin1("    int m_methodArgCount[%1];").arg(methodCount) << Qt::endl;
        out << QString::fromLatin1("    const int* m_methodArgTypes[%1];").arg(methodCount) << Qt::endl;
    }
    out << QStringLiteral("};") << Qt::endl;
    out << "" << Qt::endl;
}

QT_END_NAMESPACE
