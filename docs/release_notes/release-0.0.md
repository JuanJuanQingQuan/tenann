# TenANN v0.0.x

## v0.0.2-RELEASE
Download URL: [tenann-v0.0.2-RELEASE.tar.gz](https://mirrors.tencent.com/repository/generic/doris_thirdparty/tenann-v0.0.2-RELEASE.tar.gz)

Release date: 2023.08.31

### New Features
- 新增索引管理通用接口：
  - Index
  - IndexMeta
  - IndexReader 
  - IndexWriter 
  - IndexBuilder
  - IndexFactory
  - Searcher 
  - AnnSearcher & AnnSearcherFactory
- 支持通用的索引元数据管理：IndexMeta
- 支持Faiss HNSW索引的构建、查询、读取、写入
- 支持通过`ArraySeqView`或`VlArraySeqView`两种输入类型构建Faiss HNSW索引
- 支持通过`Build`或`BuildWithPrimaryKey`两种方式构建构建Faiss HNSW索引
- 支持文件级别的LRU索引缓存
- 支持全部依赖打包发布为`libtenann-bundle.a`
- 新增Error和FatalError异常类，用于区分可恢复和不可恢复的错误
- 新增T_LOG系列宏，支持简单的日志输出并根据日志级别抛出错误
- 新增部分单元测试

## v0.0.2-RC3
Download URL: [tenann-v0.0.2-RC3.tar.gz](https://mirrors.tencent.com/repository/generic/doris_thirdparty/tenann-v0.0.2-RC3.tar.gz)

Release date: 2023.08.31

### New Features
- 增加了构建docker镜像的脚本

### Build Changes
- 删除了cmake find_package的支持，简化了CMakeLists
- CMkaeLists中不再硬编码tenann依赖的faiss路径，而是由环境变量`TENANN_THIRDPARTY`指定
- lapack和blas不再由yum安装，改为源码依赖
- 除glibc之外的全部依赖改为静态链接
- 将所有静态库依赖和`libtenann.a`一起打包至`libtenann-bundle.a`
- 发布目录的结构修改为：
```
output
├── include
│   └── tenann
└── lib
    └── libtenann-bundle.a
```

## v0.0.2-RC2

Download URL: [tenann-v0.0.2-RC2.tar.gz](https://mirrors.tencent.com/repository/generic/doris_thirdparty/tenann-v0.0.2-RC2.tar.gz)

Release date: 2023.08.29

### API Changes
- `IndexBuilder.SetIndexWriter`接收的类型为`IndexWriterRef`（`shared_ptr<IndexWriter>`），不再接收裸指针
- `Searcher.SetIndexReader`接收的类型为`IndexReaderRef`（`shared_ptr<IndexReader>`），不再接收裸指针

### New Features
- IndexBuilder支持VlArraySeqView类型输入
- IndexBuilder支持BuildWithPrimaryKey

### Improvements
- 所有Faiss Index共享相同的IndexReader/Writer

### Bug Fixes
- 修复v0.0.2-RC1引入的Faiss构建失败问题