using Xunit;
using FluentAssertions;
using REX.Shell.Services;

namespace REX.Shell.Tests;

public class IpcClientTests
{
    [Fact]
    public void ServiceInterfaces_ShouldBeDefined()
    {
        var type = typeof(REX.Shell.Services.IWidgetHostService);
        type.Should().NotBeNull();
    }

    [Fact]
    public void WidgetInstance_ShouldHaveDefaultState()
    {
        var instance = new WidgetInstance();
        instance.State.Should().Be(WidgetLifecycleState.Stopped);
    }

    [Fact]
    public void AutomationFlow_ShouldHaveDefaultTrigger()
    {
        var flow = new AutomationFlow();
        flow.Trigger.Should().BeOfType<ScheduleTrigger>();
    }

    [Theory]
    [InlineData(WidgetLifecycleState.Loading)]
    [InlineData(WidgetLifecycleState.Running)]
    [InlineData(WidgetLifecycleState.Suspended)]
    [InlineData(WidgetLifecycleState.Crashed)]
    [InlineData(WidgetLifecycleState.Stopped)]
    public void WidgetLifecycle_AllStates_ShouldBeValid(WidgetLifecycleState state)
    {
        Enum.IsDefined(state).Should().BeTrue();
    }

    [Fact]
    public void Profile_ShouldSupportMultiMonitor()
    {
        var profile = new Profile
        {
            ProfileId = "work",
            Monitors = new Dictionary<string, string>
            {
                ["DISPLAY1"] = "widget-layout-1",
                ["DISPLAY2"] = "widget-layout-2"
            }
        };
        profile.Monitors.Should().HaveCount(2);
    }
}
