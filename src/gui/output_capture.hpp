#pragma once
#include <QObject>
#include <QtLogging>
#include <thread>
namespace jxrc::gui {
class OutputCapture final : public QObject {
    Q_OBJECT
public:
    static OutputCapture* instance();
    ~OutputCapture() override;
signals:
    void textReady(const QString& text);
private:
    explicit OutputCapture(QObject* parent);
    int savedOut_=-1,savedErr_=-1,out_=-1,err_=-1;
    std::thread reader_;
    QtMessageHandler previous_=nullptr;
    static void qtMessage(QtMsgType,const QMessageLogContext&,const QString&);
};
}
