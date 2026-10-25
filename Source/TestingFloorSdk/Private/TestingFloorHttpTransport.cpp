#include "TestingFloor/TestingFloorClient.h"

#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

namespace TestingFloor::Private
{
class FHttpTransport final : public ITransport
{
public:
	virtual ~FHttpTransport() override
	{
		for (const FHttpRequestPtr& Request : Requests)
		{
			Request->OnProcessRequestComplete().Unbind();
			Request->CancelRequest();
		}
	}

	virtual void Send(const FString& Endpoint, const FString& Body, TFunction<void(int32, bool)> Completion) override
	{
		Requests.RemoveAll([](const FHttpRequestPtr& Request) { return Request->GetStatus() != EHttpRequestStatus::Processing; });
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Requests.Add(Request);
		Request->SetURL(Endpoint);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetContentAsString(Body);
		Request->SetTimeout(10.0f);
		Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
		const TSharedRef<TFunction<void(int32, bool)>> Callback = MakeShared<TFunction<void(int32, bool)>>(MoveTemp(Completion));
		Request->OnProcessRequestComplete().BindLambda([Callback](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded) {
			(*Callback)(Response.IsValid() ? Response->GetResponseCode() : 0, bSucceeded);
		});
		if (!Request->ProcessRequest())
		{
			Request->OnProcessRequestComplete().Unbind();
			(*Callback)(0, false);
		}
	}

	virtual void Pump(float DeltaSeconds) override
	{
		FHttpModule::Get().GetHttpManager().Tick(DeltaSeconds);
	}

private:
	TArray<FHttpRequestPtr> Requests;
};
} // namespace TestingFloor::Private

TSharedRef<TestingFloor::ITransport> TestingFloor::MakeHttpTransport()
{
	return MakeShared<Private::FHttpTransport>();
}
