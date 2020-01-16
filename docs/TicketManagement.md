
Introduction
============

This document describes extra data required for issues and pull requests.

As it is currently not much supported from the hosting platform, the project
is using labels to provide some specific data. Labels are also categorized
so that the area of interest is clear, however users must pay attention on
the use, as it's not guarded automatically.

There are two general types of labels:

1. With symbolic marker: multiple labels of that kind can be used.
2. With category prefix: only one of them should be used at a time.


Prioritization
==============

Although there's a category of "Priority", it's only additional data that
define the priority of interest and it's at the lowest level of interest.
Other things that define prioritization are:

1. Type: the **Bug** type tickets are of highest priority. This means that
this can only be superseded by **Bug** tickets that have higher priority.
Another important type is **Maintenance**, which should get interest over
**Enhancement**, although these things are expected to be quickly resolved.
2. The **[core]** scope is more important than anything else.
3. The **Impact** category may be significant for the works. The **Low**
impact category is expected to be quick to review and therefore it can
be treated as priority enhancement.
4. The upcoming release gets highest priority. The "Priority" category
then decides priorities only within the release, while as the deadline
for the release comes closer, lowest priority (according to all above
criteria) should be the first to potentially shift to the next release.


Category: [scope]
=================

This embraces labels in square brackets. Currently available are:

* [apps]: user applications (files usually in apps directory)
* [build]: build system (CMakeLists.txt and around)
* [core]: core SRT library
* [devel]: development applications (testing dir) and examples
* [docs]: documentation only (also changes in comments in the code)
* [interop]: interoperability with other libraries
* [platform]: platform-specific problems or enhancements
* [tests]: unit tests code (test directory)


Category: Type
==============

This describes what the issue or PR is all about:

* Bug:
   * Issue: describes a bug in the code that points out unwanted behavior
   * PR: fixes a bug
* Enhancement: Requests/provides an enhanced functionality
* Maintenance: Concerns typically development-oriented fixes
* Question: (Issue only): Report of an unclear behavior.


Category: Status
================

This describes the current status of a PR or Issue and defines what
should be done next with it.

* **Abandoned** : The reported problem is of no more interest.
   * Typically after 3 months of inactivity it will be closed.
* **Accepted** : For issue of type Question, a satisfactionary answer
   * Expected the question can be closed, although it still remains archived
* **Available** : A requested enhancement could be provided without extensions.
   * Expected to be closed once picked up by the requester.
* **Blocked** : A dependent Issue/PR must be closed/merged first.
   * Should stay unchanged until the dependency is resolved
   * NOTE: The first line in Description must provide dependency info!
* **Completed** : Ready to close, if there's nothing else.
   * Typically after 3 months of inactivity it will be closed.
* **Fixing After Review** : review was done, expected post-review fixes to be done
   * Aftera too long inactivity it will be `->`takenover
* **In Progress** : The work is ongoing and not yet ready to review
* **Moved** : Duplicate or takeover by another Issue/PR
   * NOTE: The first line in Description must provide redirection info!
* **On Hold** : The work was paused due to unknown reasons.
   * Should be revisited after every release
* **Pending** : Not started, but recognized as needed to do.
   * Expected to turn into "In Progress" once resources are available
* **Review needed** : expected review to be performed
* **Unclear** : The problem can't be reproduced or is incorrectly described
   * Expected clarification from the reporter
   * After a too long inactivity it will be `->`takenover

`->`takenover tickets are tickets that have their original reported
considered lost, therefore there are two possible actions:

* If the ticket describes or provides something important for the project,
especially if it's about an importnat functionality fix, or an enhancement
in the interest of the project management, a new ticket will be spawned
to handle it, this time without involving the original reporter and resolved
within the project team exclusively.

* After the takeover, or if it was decided that the takeover wasn't necessary
and 3 months have passed, the ticket is closed.

Note that the status typically defines something that is expected to
happen next. Please note also that the 3-month quarantine is enforced
manually, so some tickets may stay longer open.

(**TO DO: A picture that describes the state machine would be nice to have**)

Category: Priority
==================

This describes the importance of the matter described in the ticket,
as a balancing data for tickes that are otherwise equal in the hierarchy
by other reasons.

Practical meaning of the priority is only within particular type. There
is a general meaning of the priorities:

* Critical: this is predicted for bugs and bugfixes only. This has the
highest possible priority as it repairs some very serious bug. Hangup of
all other works is expected to investigate the reported issue, as well as
review and merge the associated ticket.

* High: The first issues to be taken out of the way, in the current
release only. Tickes of high priority are **not** expected to be moved
to the next release, unless a "quick bugfix release" is announced.

* Medium: The next series that should be taken after all **High**
priority tickets are resolved. Those that can't be resolved in this
release, should be moved to the next one.

* Low: Something that didn't gain enough interest to be important,
only applicable if **Medium** tickets are resolved.


Category: Impact
================

This is for PRs only. It describes the character of changes provided
by the PR as to how much they influence the current state of the
project's resources (both code and documentation).

* Low: Usually it's a refactoring, code reformatting, or something
equally insignificant for the functionality.

* Optional: Provides a significant set of changes, but only as an
experimental feature that is off by default (usually requires to
be turned on by a build option, otherwise provides no changes in
the functionality)

* Development: Provides a significant set of changes that concern
only development support parts of the code (such as logging). This
should not touch the general functionality at all.

* Significant: Provides a significant set of changes in the main
functionality, which may change the behavior of the basic functionalities,
however the existing functionalities are expected to not be impaired
or altered.

* High: This can potentially provide changes that can highly impact
the existing functionalities and existing functionalities are also
expected to work quite differently with these changes applied.

Important information provided by this label is how much the reviewers
should expect the given PR to change, as well as what they should
focus on. For example, PRs of Low impact should be expected to be
processed quickly, after the reviewer is certain this information
isn't mistaken, while those with High impact may require extra tests
and thorough confirmation.


