locals {
  placeholder_hostname = "placeholder-hostname-to-rewrite-in-nginx"
  log_config = {
    logDriver = "awslogs"
    options = {
      awslogs-group         = aws_cloudwatch_log_group.fargate_logging.name
      awslogs-region        = var.region_name
      awslogs-stream-prefix = "ecs"
    }
  }
  misp_container_name = "misp"
  container_definition = [
    {
      image     = var.misp_image
      name      = "misp-core"
      essential = true
      portMappings = [{
        containerPort = 50000
        hostPort      = 50000
        protocol      = "tcp"
        }, {
        containerPort = 80
        hostPort      = 80
        protocol      = "tcp"
      }],
      volumesFrom = []
      environment = [{
        name  = "HOSTNAME"
        value = "https://${local.placeholder_hostname}"
        }, {
        name  = "REDIS_FQDN"
        value = "localhost"
        }, {
        name  = "INIT"
        value = "true"
        }, {
        name  = "NOREDIR"
        value = "true"
        }, {
        name  = "CRON_USER_ID"
        value = "1"
        }, {
        name  = "MYSQL_HOST"
        value = "127.0.0.1"
        }, {
        name  = "MYSQL_USER"
        value = "demo"
        }, {
        name  = "MYSQL_PASSWORD"
        value = "demo"
        }, {
        name  = "MYSQL_DATABASE"
        value = "misp"
        }, {
        name  = "DISIPV6"
        value = "true"
        }, {
        name  = "WORKERS"
        value = "1"
        }, {
        name  = "MISP_ADMIN_USER"
        value = "demo@tenzir.com"
        }, {
        name  = "MISP_ADMIN_PASSWORD"
        value = "demo"
        }, {
        name  = "MISP_API_KEY"
        value = "demodemodemodemodemodemodemodemodemodemo"
      }]
      logConfiguration = local.log_config
      depends_on = [
        {
          containerName = "redis",
          condition     = "HEALTHY"
        },
        {
          containerName = "mysql",
          condition     = "HEALTHY"
        }
      ]
    },
    {
      image     = "coolacid/misp-docker:modules-${var.misp_version}"
      name      = "misp-modules"
      essential = true
      environment = [{
        name  = "REDIS_BACKEND"
        value = "localhost"
      }]
      logConfiguration = local.log_config
      depends_on = [
        {
          containerName = "redis",
          condition     = "HEALTHY"
        },
        {
          containerName = "mysql",
          condition     = "HEALTHY"
        }
      ]
    },
    {
      image     = "mysql:${local.mysql_version}"
      name      = "mysql"
      essential = true
      command : ["--default-authentication-plugin=mysql_native_password"]
      capabilities = {
        add = ["SYS_NICE"]
      }
      mountPoints = [
        {
          sourceVolume  = "storage",
          containerPath = "/var/lib/mysql",
          readOnly      = false
        }
      ]
      environment = [{
        name  = "MYSQL_USER"
        value = "demo"
        }, {
        name  = "MYSQL_PASSWORD"
        value = "demo"
        }, {
        name  = "MYSQL_ROOT_PASSWORD"
        value = "demo"
        }, {
        name  = "MYSQL_DATABASE"
        value = "misp"
      }]
      logConfiguration = local.log_config
    },
    {
      image            = "redis:${local.redis_version}"
      name             = "redis"
      essential        = true
      logConfiguration = local.log_config
    },
    {
      image     = var.misp_proxy_image
      name      = "misp-proxy"
      essential = true
      portMappings = [{
        containerPort = 8080
        hostPort      = 8080
        protocol      = "tcp"
      }]
      environment = [{
        name  = "NGINX_PLACEHOLDER_HOSTNAME"
        value = local.placeholder_hostname
      }, {
        name  = "NGINX_PORT"
        value = "8080"
      }, {
        name  = "NGINX_PROXY_PASS"
        value = "http://localhost:80"
      }]
    }
    # we don't instantiate the smtp server
  ]
}
