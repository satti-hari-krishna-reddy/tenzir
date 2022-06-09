# About VAST

:::tip Get Help
Welcome to the VAST documentation! If you have any questions, do not hesitate to
join our [community Slack](http://slack.tenzir.com) or open a [GitHub
discussion](https://github.com/tenzir/vast/discussions).
:::

## What is VAST?

VAST is an embeddable telemetry engine for structured event data, purpose-built
for use cases in security operations.

### Use Cases

Consider VAST if you want to:

- Store, aggregate, and manage massive amounts of security telemetry
- BYO data science and data engineering tools for security analytics
- Build a foundation for a federated detection and response architecture
- Operationalize threat intelligence and detect at the edge
- Empower threat hunters with a data-centric investigation tool

If you're unsure whether VAST is the right tool for your use case, keep reading.

## Why VAST?

VAST fills a gap when you need a highly embeddable database for security
data that powers detection and response use cases. The following graphic
illustrates the placement of VAST in the spectrum of *Observability* ⇔
*Security* and *Open Platform* ⇔ *Data Silo*.

![VAST Spectra](/img/ecosystem.png)

There exist a lot of database systems out there, and you may ask yourself the
question: why not use that other system instead? To help answer this question,
we offer a brief guidance below when other systems might be a better fit.

### VAST vs. SIEMs

Traditional SIEMs support basic search and a fixed set of analytics operations.
Most systems face scaling issues and are therefore limited for comprehensive
analysis that includes high-volume telemetry. They also lack good support for
threat hunting and raw exploratory data analysis. Consequently, advanced
emerging use cases, such as data science and detection engineering, require
additional data-centric workbenches.

VAST *complements* a [SIEM][siem] nicely with the following use cases:

- **Offloading**: route the high-volume telemetry to VAST that would otherwise
  overload your SIEM or be cost-prohibitive to ingest. By keeping the bulk of
  the data in VAST, you remove bottlenecks and can selectively forward the
  activity that matters to your SIEM.

- **Compliance**: VAST supports fine-grained retention configuration to meet
  [GDPR](https://en.wikipedia.org/wiki/General_Data_Protection_Regulation) and
  other regulatory requirements. When storage capacity needs careful management,
  VAST's *compaction* feature allows for weighted ageing of your data, so that
  you can specify relative importance of event types. Powerful *transforms*
  allow you to anonymize, pseudonymize, or encrypt specific fields—either to
  sanitize [PII data](https://en.wikipedia.org/wiki/Personal_data) on import, or
  ad-hoc on export when data leaves VAST.

- **Data Science**: The majority of SIEMs provide an API-only, low-bandwidth
  access path to your security data. VAST is an [Arrow][arrow]-native engine
  that offers unfettered high-bandwidth access so that you can bring your own
  workloads, with your own tools, e.g., to run iterative clustering algorithms
  or complex feature extraction in conjunction with machine learning.

[siem]: https://en.wikipedia.org/wiki/Security_information_and_event_management
[arrow]: https://arrow.apache.org

:::note Recommendation
Unlike a heavy-weight legacy SIEM, VAST is highly embeddable so that you can
run it everywhere: containerized in the public cloud, on bare-metal appliances
deep in the network, or at the edge.
:::

### VAST vs. Data Warehouses

Data warehouses,
[OLAP](https://en.wikipedia.org/wiki/Online_analytical_processing) engines, and
time series databases seem like an appealing choice for immutable structured
data. They offer sufficient ingest bandwidth, perform well on group-by and
aggregation queries, come frequently with advanced operations like joins, and
often scale out well.

However, as a cornerstone for security operations, they fall short in supporting
the following relevant use cases where VAST has the edge:

- **Data Onboarding**: it takes considerable effort to write and maintain
  schemas for the tables of the respective data sources. Since VAST is
  purpose-built for security data, integrations for key data sources and data
  carriers exist out of the box.

- **Rich Typing**: modeling security event data with a generic database often
  reduces the values to strings or integers, as opposed to retaining
  domain-specific semantics, such as IP addresses or port numbers. VAST offers a
  rich type system that can retain such semantics, supporting both
  *schema-on-read* (taxonomies) and *schema-on-write* (transforms).

- **Fast Search**: typical query patterns are (1) automatically triggered point
  queries for tactical threat intelligence, arriving at a high rate and often in
  bulk, of which the majority are true negatives, (2) regular expression search
  for finding patterns in command line invocations, URLs, or opaque string
  messages, and (3) group-by and aggregations when hunting for threats or when
  performing threshold-based detections. Data warehouses work well for (3) but
  rarely for (1) and (2) as well.

:::note Recommendation
Data warehouses may be well-suited for raw data processing, but a data backbone
for security operations has a lot more domain-specific demands. The required
heavy lifting to bridge this gap is cost and time prohibitive for any security
operations center. This is why we built VAST.
:::

### VAST vs. Relational DBs

Unlike [OLAP](#vast-vs-data-warehouses) workloads,
[OLTP](https://en.wikipedia.org/wiki/Online_transaction_processing) workloads
have strong transactional and consistency guarantees, e.g., when performing
inserts, updates, and deletes. These extra guarantees come at a cost of
throughput and latency when working with large datasets, but are rarely needed
in security analytics (e.g., ingestion is an append-only operation). In a domain
of incomplete data, VAST trades correctness for performance and availability,
i.e., throttles a data source with backpressure instead of falling behind and
risking out-of-memory scenarios.

:::note Recommendation
If you aim to perform numerous modifications on a small subset of event data,
with medium ingest rates, relational databases, like PostgreSQL or MySQL, might
be a better fit. VAST's columnar data representation is ill-suited for row-level
modifications.
:::

### VAST vs. Document DBs

Document DBs, such as MongoDB, offer worry-free ingestion of unstructured
data. They scale well horizontally and flexible querying.

However, they might not be the best choice for the data plane in security
operations, for the following reasons:

- **Vertical Scaling**: when co-locating a storage engine next to high-volume
  data sources, e.g., on a network appliance together with a network monitor,
  CPU and memory constraints coupled with a non-negligible IPS overhead prohibit
  scaling horizontally to build a "cluster in a box."

- **Analytical Workloads**: the document-oriented storage does not perform well
  for analytical workloads, such as group-by and aggregation queries. But such
  analytics are very common in interactive threat hunting scenarios and in
  various threshold-based detections. VAST leverages Arrow for columnar data
  representation and partially for query execution.

- **Economy of Representation**: security telemetry data exhibits a lot of
  repetitiveness between events, such as similar IP addresses, URL prefixes, or
  log message formats. This data compresses much better when transposed into a
  columnar format, such as Parquet.

A special case of document DBs are full-text search engines, such as
ElasticSearch or Solr. The unit of input is typically unstructured text. The
search engine uses (inverted) indexes and ranking methods to return the most
relevant results for a given combination of search terms.

:::note Recommendation
Most of the security telemetry arrives as structured log/event data, as opposed
to unstructured textual data. If your primary use case involves working with
text, VAST might not be a good fit. That said, needle-in-haystack search
and other information retrieval techniques are still relevant for security
analytics, for which VAST has basic support.
:::

### VAST vs. Timeseries DBs

Timeseries databases share a lot in common with [OLAP
engines](#vast-vs-data-warehouses), but put center data organization around
time.

:::note Recommendation
If you plan to access your event data through time domain and need to model the
majority of data as series, a timeseries DBs may suit the bill. If you access
data through other (spatial) attributes, like IP addresses or domains, a
traditional timeseries DB might not be good fit—especially for high-cardinality
attributes. If your analysis involve running more complex detections, or
include needle-in-haystack searches, VAST might be a better fit.
:::

### VAST vs. Key-Value DBs

A key-value store performs a key-based point or range lookup to retrieve one or
more values. Security telemetry is high-dimensional data and there are many more
desired entry points than a single key besides time, e.g., IP address,
application protocol, domain name, or hash value.

:::note Recommendation
Key-value stores alone are not suitable as foundation for running security
analytics workloads. There are narrow use cases where key-value stores can
facilitate certain capabilities, e.g., when processing watch lists. (VAST offers
a *matcher* plugin for this purpose.)
:::

### VAST vs. Graph DBs

Graph databases are purpose-built for answering complex queries over networks of
nodes and their relationships, such as finding shortest paths, measuring node
centrality, or identifying connected components. While networks and
communication patterns can naturally be represented as graphs, traditional
security analytics query patterns may not benefit from a graph representation.

:::note Recommendation
If graph-centric queries dominate your use case, VAST is not the right execution
engine. VAST can still prove valuable as foundation for graph analytics by
storing the raw telemetry and feeding it (via Arrow) into graph engines that
support ad-hoc data frame analysis.
:::

## What's Next?

We organize the remainder of this documentation along the journey of a typical
user:

1. [Setup VAST](/docs/setup-vast) describes how you can download, install, and
   configure VAST in a variety of environments.
   👉 *Start here if you want to deploy VAST.*
2. [Use VAST](/docs/use-vast) explains how to work with VAST, e.g., ingesting
   data, running queries, matching threat intelligence, or integrating it with
   other security tools.
   👉 *Go here if you have a running VAST, and want to explore what you can do
   with it.*
3. [Understand VAST](/docs/understand-vast) describes the system design goals
   and architecture, e.g., the actor model as concurrency and distribution
   layer, separation of read/write path, and core components like the catalog
   that provides light-weight indexing and manages schema meta data.
   👉 *Read here if you want to know why VAST is built the way it is.*
4. [Develop VAST](/docs/develop-vast) provides developer-oriented resources to
   work on VAST, e.g., write own plugins or enhance the source code.
   👉 *Look here if you are ready to get your hands dirty and write code.*

Ready to take VAST for spin? Turn the page for a quick hands-on walk-through.