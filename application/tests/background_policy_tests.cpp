#include "background_policy.h"
#include <QtTest>
class BackgroundPolicyTests : public QObject {
    Q_OBJECT
  private slots:
    void pressureRequiresSustainedLoadAndRecovery() {
        PressureGate gate;
        QVERIFY(!gate.observe(.9));
        QVERIFY(gate.observe(.85));
        QVERIFY(gate.observe(.6));
        QVERIFY(gate.observe(.2));
        QVERIFY(gate.observe(.3));
        QVERIFY(!gate.observe(.1));
        QVERIFY(!gate.observe(.9));
        QVERIFY(!gate.observe(.6));
        QVERIFY(!gate.observe(.9));
        QVERIFY(gate.observe(.99));
        gate.reset();
        QVERIFY(!gate.busy());
    }
    void unavailableSamplesDoNotTurnIntoLoad() {
        PressureGate gate;
        QVERIFY(!gate.observe(-1));
        QVERIFY(!gate.observe(2));
        gate.observe(.9);
        gate.observe(.9);
        QVERIFY(gate.observe(-1));
    }
    void systemSamplerFirstSampleIsUnavailableThenBounded() {
        SystemLoadSampler sampler;
        QCOMPARE(sampler.sample(), -1.0);
        QTest::qWait(30);
        const auto value = sampler.sample();
        QVERIFY(value == -1 || (value >= 0 && value <= 1));
        sampler.reset();
        QCOMPARE(sampler.sample(), -1.0);
    }
};
QTEST_GUILESS_MAIN(BackgroundPolicyTests)
#include "background_policy_tests.moc"
