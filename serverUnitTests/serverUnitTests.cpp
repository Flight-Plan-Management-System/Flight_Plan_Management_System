#include "pch.h"
#include "CppUnitTest.h"
#include <string>
#include <vector>
#include <memory>
using namespace Microsoft::VisualStudio::CppUnitTestFramework;
#include "C:\Users\JIGNESH PATEL\OneDrive\Desktop\FlightPlan\FlightPlan\server.cpp"
namespace serverUnitTests
{
	TEST_CLASS(serverUnitTests)
	{
	public:
        TEST_METHOD(Copy_ValidInputs_Success)
        {
            char dest[10];
            const char* src = "test";

            auto result = SafeString::copy(dest, sizeof(dest), src);

            Assert::AreEqual(static_cast<int>(ServerStateMachine::SUCCESS), static_cast<int>(result));
            Assert::AreEqual(src, dest);
        }
	};
}
